//! Durable, local-only people organization keyed to Catalog photo identities.
//!
//! Model embeddings remain request-local. This sidecar stores only the bounded
//! occurrence references required to retain anonymous groups and explicit user
//! merges across desktop sessions. Keeping it beside, rather than inside, the
//! Catalog lets the user clear or rebuild people data independently.

use std::{
    collections::{BTreeMap, BTreeSet, HashMap},
    fs,
    path::PathBuf,
    sync::Mutex,
};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use rusqlite::{Connection, OptionalExtension, Transaction, params};
use shadow_ai::FaceOccurrenceReference;
use shadow_core::PeopleAnalysisReport;

const SCHEMA_VERSION: i64 = 2;
const SCHEMA_IDENTITY: &str = "shadow-people-store-20260831.2";
const LEGACY_SCHEMA_VERSION: i64 = 1;
const LEGACY_SCHEMA_IDENTITY: &str = "shadow-people-store-20260830.1";
const MAX_DISPLAY_NAME_BYTES: usize = 256;

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct PeopleLibraryGroup {
    pub(crate) person_id: String,
    pub(crate) display_name: String,
    pub(crate) member_count: u32,
    pub(crate) photo_ids: Vec<String>,
    pub(crate) thumbnail_jpeg: Vec<u8>,
    pub(crate) manually_merged: bool,
}

#[derive(Debug, Clone, PartialEq)]
pub(crate) struct PeopleLibrarySnapshot {
    pub(crate) has_data: bool,
    pub(crate) analyzed_photos: u32,
    pub(crate) detected_faces: u32,
    pub(crate) embedded_faces: u32,
    pub(crate) skipped_items: u32,
    pub(crate) ungrouped_faces: u32,
    pub(crate) truncated: bool,
    pub(crate) grouping_revision: String,
    pub(crate) groups: Vec<PeopleLibraryGroup>,
    pub(crate) can_undo_merge: bool,
}

impl PeopleLibrarySnapshot {
    pub(crate) fn empty() -> Self {
        Self {
            has_data: false,
            analyzed_photos: 0,
            detected_faces: 0,
            embedded_faces: 0,
            skipped_items: 0,
            ungrouped_faces: 0,
            truncated: false,
            grouping_revision: String::new(),
            groups: Vec::new(),
            can_undo_merge: false,
        }
    }
}

#[derive(Debug)]
pub(crate) struct PeopleLibraryStore {
    state: Mutex<PeopleLibraryState>,
}

#[derive(Debug)]
struct PeopleLibraryState {
    connection: Connection,
    undo: Option<StoredSnapshot>,
}

#[derive(Debug, Clone)]
struct StoredSnapshot {
    analyzed_photos: u32,
    detected_faces: u32,
    embedded_faces: u32,
    skipped_items: u32,
    ungrouped_faces: u32,
    truncated: bool,
    grouping_revision: String,
    groups: Vec<StoredGroup>,
}

#[derive(Debug, Clone)]
struct StoredGroup {
    person_id: String,
    sort_index: u32,
    display_name: String,
    thumbnail_jpeg: Vec<u8>,
    manually_merged: bool,
    occurrences: Vec<StoredOccurrence>,
}

#[derive(Debug, Clone)]
struct StoredOccurrence {
    occurrence_id: String,
    photo_id: String,
    representation_id: String,
    bounds: [f32; 4],
}

impl PeopleLibraryStore {
    pub(crate) fn open(root: impl Into<PathBuf>) -> AnyResult<Self> {
        let root = root.into();
        fs::create_dir_all(&root)
            .with_context(|| format!("create local people store root {}", root.display()))?;
        let path = root.join("people.sqlite");
        let mut connection = Connection::open(&path)
            .with_context(|| format!("open local people store {}", path.display()))?;
        connection
            .busy_timeout(std::time::Duration::from_secs(2))
            .context("configure local people store busy timeout")?;
        initialize(&mut connection)?;
        Ok(Self {
            state: Mutex::new(PeopleLibraryState {
                connection,
                undo: None,
            }),
        })
    }

    pub(crate) fn snapshot(&self) -> AnyResult<PeopleLibrarySnapshot> {
        let state = self.lock()?;
        match read_snapshot(&state.connection)? {
            Some(stored) => project_snapshot(stored, state.undo.is_some()),
            None => Ok(PeopleLibrarySnapshot::empty()),
        }
    }

    pub(crate) fn replace_analysis(
        &self,
        report: PeopleAnalysisReport,
    ) -> AnyResult<PeopleLibrarySnapshot> {
        let mut state = self.lock()?;
        let existing = read_snapshot(&state.connection)?.unwrap_or_else(StoredSnapshot::empty);
        let next = reconcile_analysis(existing, report)?;
        write_snapshot(&mut state.connection, &next)?;
        state.undo = None;
        project_snapshot(next, false)
    }

    pub(crate) fn merge_people(&self, person_ids: &[String]) -> AnyResult<PeopleLibrarySnapshot> {
        let selected = person_ids.iter().cloned().collect::<BTreeSet<_>>();
        if selected.len() < 2 {
            bail!("at least two distinct people are required for a merge");
        }
        let mut state = self.lock()?;
        let mut snapshot = read_snapshot(&state.connection)?
            .ok_or_else(|| anyhow!("people data has not been analyzed"))?;
        let selected_indexes = snapshot
            .groups
            .iter()
            .enumerate()
            .filter_map(|(index, group)| selected.contains(&group.person_id).then_some(index))
            .collect::<Vec<_>>();
        if selected_indexes.len() != selected.len() {
            bail!("one or more selected people are no longer available");
        }

        let mut photo_ids = BTreeSet::new();
        for index in &selected_indexes {
            for occurrence in &snapshot.groups[*index].occurrences {
                if !photo_ids.insert(occurrence.photo_id.clone()) {
                    bail!("people with faces in the same photo cannot be merged");
                }
            }
        }

        let target_index = *selected_indexes
            .iter()
            .min_by_key(|index| snapshot.groups[**index].sort_index)
            .expect("selected people were validated as non-empty");
        let target_person_id = snapshot.groups[target_index].person_id.clone();
        let target_sort_index = snapshot.groups[target_index].sort_index;
        let target_thumbnail = snapshot.groups[target_index].thumbnail_jpeg.clone();
        let target_display_name = selected_indexes
            .iter()
            .map(|index| &snapshot.groups[*index])
            .min_by_key(|group| group.sort_index)
            .and_then(|group| (!group.display_name.is_empty()).then(|| group.display_name.clone()))
            .or_else(|| {
                selected_indexes
                    .iter()
                    .map(|index| &snapshot.groups[*index])
                    .filter(|group| !group.display_name.is_empty())
                    .min_by_key(|group| group.sort_index)
                    .map(|group| group.display_name.clone())
            })
            .unwrap_or_default();
        let mut merged_occurrences = Vec::new();
        for index in &selected_indexes {
            merged_occurrences.extend(snapshot.groups[*index].occurrences.clone());
        }
        merged_occurrences.sort_by(|left, right| left.occurrence_id.cmp(&right.occurrence_id));
        snapshot
            .groups
            .retain(|group| !selected.contains(&group.person_id));
        snapshot.groups.push(StoredGroup {
            person_id: target_person_id,
            sort_index: target_sort_index,
            display_name: target_display_name,
            thumbnail_jpeg: target_thumbnail,
            manually_merged: true,
            occurrences: merged_occurrences,
        });
        normalize_group_order(&mut snapshot.groups);

        let previous =
            read_snapshot(&state.connection)?.expect("the people snapshot was read before merging");
        write_snapshot(&mut state.connection, &snapshot)?;
        state.undo = Some(previous);
        project_snapshot(snapshot, true)
    }

    pub(crate) fn rename_person(
        &self,
        person_id: &str,
        display_name: &str,
    ) -> AnyResult<PeopleLibrarySnapshot> {
        let display_name = normalized_display_name(display_name)?;
        let mut state = self.lock()?;
        let mut snapshot = read_snapshot(&state.connection)?
            .ok_or_else(|| anyhow!("people data has not been analyzed"))?;
        let group = snapshot
            .groups
            .iter_mut()
            .find(|group| group.person_id == person_id)
            .ok_or_else(|| anyhow!("the selected person is no longer available"))?;
        if group.display_name == display_name {
            return project_snapshot(snapshot, state.undo.is_some());
        }
        group.display_name = display_name;
        write_snapshot(&mut state.connection, &snapshot)?;
        // Rename follows the merge in the same linear correction history. A
        // stale merge undo would otherwise silently discard the new name.
        state.undo = None;
        project_snapshot(snapshot, false)
    }

    pub(crate) fn undo_merge(&self) -> AnyResult<PeopleLibrarySnapshot> {
        let mut state = self.lock()?;
        let previous = state
            .undo
            .take()
            .ok_or_else(|| anyhow!("there is no people merge to undo"))?;
        if let Err(error) = write_snapshot(&mut state.connection, &previous) {
            state.undo = Some(previous);
            return Err(error);
        }
        project_snapshot(previous, false)
    }

    pub(crate) fn clear(&self) -> AnyResult<()> {
        let mut state = self.lock()?;
        let transaction = state
            .connection
            .transaction()
            .context("begin local people-data clear")?;
        clear_snapshot_tables(&transaction)?;
        transaction
            .commit()
            .context("commit local people-data clear")?;
        state.undo = None;
        Ok(())
    }

    fn lock(&self) -> AnyResult<std::sync::MutexGuard<'_, PeopleLibraryState>> {
        self.state
            .lock()
            .map_err(|_| anyhow!("local people store state is poisoned"))
    }
}

impl StoredSnapshot {
    fn empty() -> Self {
        Self {
            analyzed_photos: 0,
            detected_faces: 0,
            embedded_faces: 0,
            skipped_items: 0,
            ungrouped_faces: 0,
            truncated: false,
            grouping_revision: String::new(),
            groups: Vec::new(),
        }
    }
}

fn initialize(connection: &mut Connection) -> AnyResult<()> {
    connection
        .execute_batch(
            "PRAGMA foreign_keys = ON;
             CREATE TABLE IF NOT EXISTS people_schema (
                 version INTEGER PRIMARY KEY NOT NULL,
                 identity TEXT NOT NULL
             ) STRICT;
             CREATE TABLE IF NOT EXISTS people_snapshot (
                 singleton INTEGER PRIMARY KEY NOT NULL CHECK (singleton = 1),
                 analyzed_photos INTEGER NOT NULL CHECK (analyzed_photos >= 0),
                 detected_faces INTEGER NOT NULL CHECK (detected_faces >= 0),
                 embedded_faces INTEGER NOT NULL CHECK (embedded_faces >= 0),
                 skipped_items INTEGER NOT NULL CHECK (skipped_items >= 0),
                 ungrouped_faces INTEGER NOT NULL CHECK (ungrouped_faces >= 0),
                 truncated INTEGER NOT NULL CHECK (truncated IN (0, 1)),
                 grouping_revision TEXT NOT NULL
             ) STRICT;
             CREATE TABLE IF NOT EXISTS people_groups (
                 person_id TEXT PRIMARY KEY NOT NULL,
                 sort_index INTEGER NOT NULL CHECK (sort_index >= 0),
                 display_name TEXT NOT NULL DEFAULT ''
                     CHECK (length(CAST(display_name AS BLOB)) <= 256),
                 thumbnail_jpeg BLOB NOT NULL,
                 manually_merged INTEGER NOT NULL CHECK (manually_merged IN (0, 1))
             ) STRICT;
             CREATE UNIQUE INDEX IF NOT EXISTS people_groups_sort_idx
                 ON people_groups(sort_index);
             CREATE TABLE IF NOT EXISTS people_occurrences (
                 occurrence_id TEXT PRIMARY KEY NOT NULL,
                 person_id TEXT NOT NULL,
                 photo_id TEXT NOT NULL,
                 representation_id TEXT NOT NULL,
                 bounds_x REAL NOT NULL,
                 bounds_y REAL NOT NULL,
                 bounds_width REAL NOT NULL,
                 bounds_height REAL NOT NULL,
                 FOREIGN KEY (person_id) REFERENCES people_groups(person_id) ON DELETE CASCADE
             ) STRICT;
             CREATE INDEX IF NOT EXISTS people_occurrences_person_idx
                 ON people_occurrences(person_id, occurrence_id);
             CREATE INDEX IF NOT EXISTS people_occurrences_photo_idx
                 ON people_occurrences(photo_id, person_id);",
        )
        .context("initialize local people store schema")?;
    let stored = connection
        .query_row(
            "SELECT version, identity FROM people_schema LIMIT 1",
            [],
            |row| Ok((row.get::<_, i64>(0)?, row.get::<_, String>(1)?)),
        )
        .optional()
        .context("read local people store schema identity")?;
    match stored {
        None => {
            connection
                .execute(
                    "INSERT INTO people_schema (version, identity) VALUES (?1, ?2)",
                    params![SCHEMA_VERSION, SCHEMA_IDENTITY],
                )
                .context("record local people store schema identity")?;
        }
        Some((SCHEMA_VERSION, identity)) if identity == SCHEMA_IDENTITY => {}
        Some((LEGACY_SCHEMA_VERSION, identity)) if identity == LEGACY_SCHEMA_IDENTITY => {
            let transaction = connection
                .transaction()
                .context("begin local people-store schema migration")?;
            transaction
                .execute_batch(
                    "ALTER TABLE people_groups
                         ADD COLUMN display_name TEXT NOT NULL DEFAULT ''
                         CHECK (length(CAST(display_name AS BLOB)) <= 256);
                     UPDATE people_schema
                         SET version = 2, identity = 'shadow-people-store-20260831.2';",
                )
                .context("migrate local people store to named people")?;
            transaction
                .commit()
                .context("commit local people-store schema migration")?;
        }
        Some((version, identity)) => bail!(
            "unsupported local people store schema {version} ({identity}); clear people data before continuing"
        ),
    }
    Ok(())
}

fn reconcile_analysis(
    existing: StoredSnapshot,
    report: PeopleAnalysisReport,
) -> AnyResult<StoredSnapshot> {
    let skipped_items = report
        .skipped
        .no_current_visual
        .saturating_add(report.skipped.unsupported_visual)
        .saturating_add(report.skipped.stale_input)
        .saturating_add(report.skipped.low_detection_confidence)
        .saturating_add(report.skipped.ineligible_embedding);
    let mut previews = report
        .group_previews
        .into_iter()
        .map(|preview| (preview.group_id, preview.thumbnail_jpeg))
        .collect::<HashMap<_, _>>();
    let existing_by_person = existing
        .groups
        .iter()
        .map(|group| (group.person_id.clone(), group))
        .collect::<HashMap<_, _>>();
    let occurrence_owner = existing
        .groups
        .iter()
        .flat_map(|group| {
            group
                .occurrences
                .iter()
                .map(move |occurrence| (occurrence.occurrence_id.clone(), group.person_id.clone()))
        })
        .collect::<HashMap<_, _>>();

    let mut drafts = BTreeMap::<String, StoredGroup>::new();
    for candidate in report.grouping.groups {
        let incoming = candidate
            .members
            .into_iter()
            .map(stored_occurrence)
            .collect::<Vec<_>>();
        let owners = incoming
            .iter()
            .filter_map(|occurrence| occurrence_owner.get(&occurrence.occurrence_id).cloned())
            .collect::<BTreeSet<_>>();
        let preview = previews.remove(&candidate.group_id).unwrap_or_default();
        if owners.len() <= 1 {
            let person_id = owners
                .iter()
                .next()
                .cloned()
                .unwrap_or_else(|| person_id_for_occurrences(&incoming));
            let prior = existing_by_person.get(&person_id).copied();
            append_draft(
                &mut drafts,
                person_id,
                incoming,
                preview,
                prior.map_or_else(String::new, |group| group.display_name.clone()),
                prior.is_some_and(|group| group.manually_merged),
                prior.map_or(u32::MAX, |group| group.sort_index),
            );
            continue;
        }

        // A model candidate that bridges people kept separate by prior user
        // organization is not allowed to silently merge them. Preserve every
        // established owner and leave only genuinely new occurrences together.
        let mut unmatched = Vec::new();
        for occurrence in incoming {
            if let Some(person_id) = occurrence_owner.get(&occurrence.occurrence_id) {
                let prior = existing_by_person
                    .get(person_id)
                    .copied()
                    .expect("occurrence owner must name an existing person");
                append_draft(
                    &mut drafts,
                    person_id.clone(),
                    vec![occurrence],
                    prior.thumbnail_jpeg.clone(),
                    prior.display_name.clone(),
                    prior.manually_merged,
                    prior.sort_index,
                );
            } else {
                unmatched.push(occurrence);
            }
        }
        if !unmatched.is_empty() {
            let person_id = person_id_for_occurrences(&unmatched);
            append_draft(
                &mut drafts,
                person_id,
                unmatched,
                preview,
                String::new(),
                false,
                u32::MAX,
            );
        }
    }

    let mut groups = drafts.into_values().collect::<Vec<_>>();
    normalize_group_order(&mut groups);
    Ok(StoredSnapshot {
        analyzed_photos: bounded_u32(report.analyzed_photos, "analyzed photo count")?,
        detected_faces: bounded_u32(report.detected_faces, "detected face count")?,
        embedded_faces: bounded_u32(report.embedded_faces, "embedded face count")?,
        skipped_items: bounded_u32(skipped_items, "skipped item count")?,
        ungrouped_faces: bounded_u32(report.grouping.ungrouped.len(), "ungrouped face count")?,
        truncated: report.truncated,
        grouping_revision: report.grouping.grouping_revision,
        groups,
    })
}

fn append_draft(
    drafts: &mut BTreeMap<String, StoredGroup>,
    person_id: String,
    occurrences: Vec<StoredOccurrence>,
    thumbnail_jpeg: Vec<u8>,
    display_name: String,
    manually_merged: bool,
    sort_index: u32,
) {
    let draft = drafts
        .entry(person_id.clone())
        .or_insert_with(|| StoredGroup {
            person_id,
            sort_index,
            display_name: display_name.clone(),
            thumbnail_jpeg: thumbnail_jpeg.clone(),
            manually_merged,
            occurrences: Vec::new(),
        });
    draft.sort_index = draft.sort_index.min(sort_index);
    draft.manually_merged |= manually_merged;
    if draft.display_name.is_empty() && !display_name.is_empty() {
        draft.display_name = display_name;
    }
    if draft.thumbnail_jpeg.is_empty() && !thumbnail_jpeg.is_empty() {
        draft.thumbnail_jpeg = thumbnail_jpeg;
    }
    draft.occurrences.extend(occurrences);
    draft
        .occurrences
        .sort_by(|left, right| left.occurrence_id.cmp(&right.occurrence_id));
    draft
        .occurrences
        .dedup_by(|left, right| left.occurrence_id == right.occurrence_id);
}

fn stored_occurrence(reference: FaceOccurrenceReference) -> StoredOccurrence {
    StoredOccurrence {
        occurrence_id: reference.occurrence_id.to_string(),
        photo_id: reference.photo_id.to_string(),
        representation_id: reference.representation_id.to_string(),
        bounds: [
            reference.bounding_box.x,
            reference.bounding_box.y,
            reference.bounding_box.width,
            reference.bounding_box.height,
        ],
    }
}

fn person_id_for_occurrences(occurrences: &[StoredOccurrence]) -> String {
    let mut ids = occurrences
        .iter()
        .map(|occurrence| occurrence.occurrence_id.as_str())
        .collect::<Vec<_>>();
    ids.sort_unstable();
    let mut hasher = blake3::Hasher::new();
    hasher.update(b"shadow.person.20260830.1");
    for id in ids {
        hasher.update(&(id.len() as u64).to_le_bytes());
        hasher.update(id.as_bytes());
    }
    format!("person-{}", hasher.finalize().to_hex())
}

fn normalize_group_order(groups: &mut [StoredGroup]) {
    groups.sort_by(|left, right| {
        left.sort_index
            .cmp(&right.sort_index)
            .then_with(|| left.person_id.cmp(&right.person_id))
    });
    for (index, group) in groups.iter_mut().enumerate() {
        group.sort_index = u32::try_from(index).unwrap_or(u32::MAX);
    }
}

fn read_snapshot(connection: &Connection) -> AnyResult<Option<StoredSnapshot>> {
    let summary = connection
        .query_row(
            "SELECT analyzed_photos, detected_faces, embedded_faces, skipped_items,
                    ungrouped_faces, truncated, grouping_revision
             FROM people_snapshot WHERE singleton = 1",
            [],
            |row| {
                Ok((
                    row.get::<_, u32>(0)?,
                    row.get::<_, u32>(1)?,
                    row.get::<_, u32>(2)?,
                    row.get::<_, u32>(3)?,
                    row.get::<_, u32>(4)?,
                    row.get::<_, bool>(5)?,
                    row.get::<_, String>(6)?,
                ))
            },
        )
        .optional()
        .context("read local people summary")?;
    let Some((
        analyzed_photos,
        detected_faces,
        embedded_faces,
        skipped_items,
        ungrouped_faces,
        truncated,
        grouping_revision,
    )) = summary
    else {
        return Ok(None);
    };

    let mut groups_statement = connection
        .prepare(
            "SELECT person_id, sort_index, display_name, thumbnail_jpeg, manually_merged
             FROM people_groups ORDER BY sort_index, person_id",
        )
        .context("prepare local people group read")?;
    let mut groups = groups_statement
        .query_map([], |row| {
            Ok(StoredGroup {
                person_id: row.get(0)?,
                sort_index: row.get(1)?,
                display_name: row.get(2)?,
                thumbnail_jpeg: row.get(3)?,
                manually_merged: row.get(4)?,
                occurrences: Vec::new(),
            })
        })
        .context("query local people groups")?
        .collect::<Result<Vec<_>, _>>()
        .context("decode local people groups")?;
    let group_indexes = groups
        .iter()
        .enumerate()
        .map(|(index, group)| (group.person_id.clone(), index))
        .collect::<HashMap<_, _>>();
    let mut occurrences_statement = connection
        .prepare(
            "SELECT occurrence_id, person_id, photo_id, representation_id,
                    bounds_x, bounds_y, bounds_width, bounds_height
             FROM people_occurrences ORDER BY person_id, occurrence_id",
        )
        .context("prepare local people occurrence read")?;
    let occurrences = occurrences_statement
        .query_map([], |row| {
            Ok((
                row.get::<_, String>(1)?,
                StoredOccurrence {
                    occurrence_id: row.get(0)?,
                    photo_id: row.get(2)?,
                    representation_id: row.get(3)?,
                    bounds: [row.get(4)?, row.get(5)?, row.get(6)?, row.get(7)?],
                },
            ))
        })
        .context("query local people occurrences")?;
    for occurrence in occurrences {
        let (person_id, occurrence) = occurrence.context("decode local people occurrence")?;
        let index = group_indexes
            .get(&person_id)
            .copied()
            .ok_or_else(|| anyhow!("local people occurrence references an unknown person"))?;
        groups[index].occurrences.push(occurrence);
    }
    Ok(Some(StoredSnapshot {
        analyzed_photos,
        detected_faces,
        embedded_faces,
        skipped_items,
        ungrouped_faces,
        truncated,
        grouping_revision,
        groups,
    }))
}

fn write_snapshot(connection: &mut Connection, snapshot: &StoredSnapshot) -> AnyResult<()> {
    let transaction = connection
        .transaction()
        .context("begin local people snapshot publication")?;
    clear_snapshot_tables(&transaction)?;
    transaction
        .execute(
            "INSERT INTO people_snapshot (
                 singleton, analyzed_photos, detected_faces, embedded_faces, skipped_items,
                 ungrouped_faces, truncated, grouping_revision
             ) VALUES (1, ?1, ?2, ?3, ?4, ?5, ?6, ?7)",
            params![
                snapshot.analyzed_photos,
                snapshot.detected_faces,
                snapshot.embedded_faces,
                snapshot.skipped_items,
                snapshot.ungrouped_faces,
                snapshot.truncated,
                snapshot.grouping_revision,
            ],
        )
        .context("write local people summary")?;
    for group in &snapshot.groups {
        transaction
            .execute(
                "INSERT INTO people_groups (
                     person_id, sort_index, display_name, thumbnail_jpeg, manually_merged
                 ) VALUES (?1, ?2, ?3, ?4, ?5)",
                params![
                    group.person_id,
                    group.sort_index,
                    group.display_name,
                    group.thumbnail_jpeg,
                    group.manually_merged,
                ],
            )
            .context("write local people group")?;
        for occurrence in &group.occurrences {
            transaction
                .execute(
                    "INSERT INTO people_occurrences (
                         occurrence_id, person_id, photo_id, representation_id,
                         bounds_x, bounds_y, bounds_width, bounds_height
                     ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                    params![
                        occurrence.occurrence_id,
                        group.person_id,
                        occurrence.photo_id,
                        occurrence.representation_id,
                        occurrence.bounds[0],
                        occurrence.bounds[1],
                        occurrence.bounds[2],
                        occurrence.bounds[3],
                    ],
                )
                .context("write local people occurrence")?;
        }
    }
    transaction
        .commit()
        .context("commit local people snapshot publication")
}

fn clear_snapshot_tables(transaction: &Transaction<'_>) -> AnyResult<()> {
    transaction
        .execute("DELETE FROM people_occurrences", [])
        .context("clear local people occurrences")?;
    transaction
        .execute("DELETE FROM people_groups", [])
        .context("clear local people groups")?;
    transaction
        .execute("DELETE FROM people_snapshot", [])
        .context("clear local people summary")?;
    Ok(())
}

fn project_snapshot(
    snapshot: StoredSnapshot,
    can_undo_merge: bool,
) -> AnyResult<PeopleLibrarySnapshot> {
    let groups = snapshot
        .groups
        .into_iter()
        .map(|group| {
            let photo_ids = group
                .occurrences
                .iter()
                .map(|occurrence| occurrence.photo_id.clone())
                .collect::<BTreeSet<_>>()
                .into_iter()
                .collect::<Vec<_>>();
            Ok(PeopleLibraryGroup {
                person_id: group.person_id,
                display_name: group.display_name,
                member_count: bounded_u32(group.occurrences.len(), "people group member count")?,
                photo_ids,
                thumbnail_jpeg: group.thumbnail_jpeg,
                manually_merged: group.manually_merged,
            })
        })
        .collect::<AnyResult<Vec<_>>>()?;
    Ok(PeopleLibrarySnapshot {
        has_data: true,
        analyzed_photos: snapshot.analyzed_photos,
        detected_faces: snapshot.detected_faces,
        embedded_faces: snapshot.embedded_faces,
        skipped_items: snapshot.skipped_items,
        ungrouped_faces: snapshot.ungrouped_faces,
        truncated: snapshot.truncated,
        grouping_revision: snapshot.grouping_revision,
        groups,
        can_undo_merge,
    })
}

fn normalized_display_name(display_name: &str) -> AnyResult<String> {
    let display_name = display_name.trim();
    if display_name.len() > MAX_DISPLAY_NAME_BYTES {
        bail!("person name exceeds {MAX_DISPLAY_NAME_BYTES} UTF-8 bytes");
    }
    if display_name.chars().any(char::is_control) {
        bail!("person name contains a control character");
    }
    Ok(display_name.to_owned())
}

fn bounded_u32(value: usize, field: &'static str) -> AnyResult<u32> {
    u32::try_from(value).with_context(|| format!("{field} exceeds the local people-store bound"))
}

#[cfg(test)]
mod tests;

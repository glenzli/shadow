//! Durable, local-only people organization keyed to Catalog photo identities.
//!
//! Model embeddings remain request-local. This sidecar stores only the bounded
//! occurrence references required to retain anonymous groups and explicit user
//! merges across desktop sessions. Keeping it beside, rather than inside, the
//! Catalog lets the user clear or rebuild people data independently.

use std::{
    collections::{BTreeSet, HashMap},
    fs,
    path::PathBuf,
    sync::Mutex,
};

use anyhow::{Context, Result as AnyResult, anyhow, bail};
use rusqlite::{Connection, OptionalExtension, Transaction, params};
use serde::{Deserialize, Serialize};
use shadow_ai::FaceOccurrenceReference;

mod history;
mod portraits;
mod reconciliation;
mod scan;
use reconciliation::reconcile_analysis;
use shadow_core::PeopleAnalysisReport;

const SCHEMA_VERSION: i64 = 4;
const SCHEMA_IDENTITY: &str = "shadow-people-store-20260922.4";
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
}

#[derive(Debug, Clone, Serialize, Deserialize)]
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

#[derive(Debug, Clone, Serialize, Deserialize)]
struct StoredGroup {
    person_id: String,
    sort_index: u32,
    display_name: String,
    thumbnail_jpeg: Vec<u8>,
    manually_merged: bool,
    #[serde(default)]
    manually_curated: bool,
    occurrences: Vec<StoredOccurrence>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
struct StoredOccurrence {
    occurrence_id: String,
    photo_id: String,
    representation_id: String,
    bounds: [f32; 4],
    #[serde(default)]
    thumbnail_jpeg: Vec<u8>,
    #[serde(default)]
    portrait_quality: f32,
    #[serde(default)]
    quality_rejected: bool,
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
            state: Mutex::new(PeopleLibraryState { connection }),
        })
    }

    pub(crate) fn snapshot(&self) -> AnyResult<PeopleLibrarySnapshot> {
        let state = self.lock()?;
        match read_snapshot(&state.connection)? {
            Some(stored) => project_snapshot(stored, history::available(&state.connection)?),
            None => Ok(PeopleLibrarySnapshot::empty()),
        }
    }

    pub(crate) fn snapshot_for_library(
        &self,
        photo_ids: &BTreeSet<String>,
    ) -> AnyResult<PeopleLibrarySnapshot> {
        let state = self.lock()?;
        let Some(mut snapshot) = read_snapshot(&state.connection)? else {
            return Ok(PeopleLibrarySnapshot::empty());
        };
        for group in &mut snapshot.groups {
            let before = group.occurrences.len();
            group
                .occurrences
                .retain(|face| photo_ids.contains(&face.photo_id));
            if before != group.occurrences.len() {
                group.thumbnail_jpeg.clear();
            }
        }
        snapshot
            .groups
            .retain(|group| !group.occurrences.is_empty());
        snapshot.analyzed_photos = bounded_u32(
            scan::checked_photos(&state.connection)?
                .intersection(photo_ids)
                .count(),
            "checked library photos",
        )?;
        project_snapshot(snapshot, history::available(&state.connection)?)
    }

    pub(crate) fn reset_analysis_progress(&self) -> AnyResult<()> {
        let mut state = self.lock()?;
        let transaction = state.connection.transaction()?;
        scan::clear(&transaction)?;
        transaction.commit()?;
        Ok(())
    }

    pub(crate) fn analysis_selection(&self) -> AnyResult<shadow_core::PeopleAnalysisSelection> {
        let state = self.lock()?;
        scan::selection(&state.connection)
    }

    pub(crate) fn replace_analysis(
        &self,
        report: PeopleAnalysisReport,
    ) -> AnyResult<PeopleLibrarySnapshot> {
        let mut state = self.lock()?;
        let stored = read_snapshot(&state.connection)?;
        if stored.is_none() && report.analyzed_photos == 0 {
            return Ok(PeopleLibrarySnapshot::empty());
        }
        let mut existing = stored.unwrap_or_else(StoredSnapshot::empty);
        if report.completed_inputs.is_empty() && report.analyzed_photos == 0 {
            existing.truncated = report.truncated;
            state.connection.execute(
                "UPDATE people_snapshot SET truncated = ?1 WHERE singleton = 1",
                [existing.truncated],
            )?;
            return project_snapshot(existing, history::available(&state.connection)?);
        }
        let mut next = reconcile_analysis(existing, report.clone())?;
        // Rebase correction history as well: undoing a rename/merge must not
        // erase photographs discovered by a subsequent analysis.
        let history = history::read_all(&state.connection)?;
        let rebased = history
            .into_iter()
            .map(|(id, snapshot)| Ok((id, reconcile_analysis(snapshot, report.clone())?)))
            .collect::<AnyResult<Vec<_>>>()?;
        let transaction = state.connection.transaction()?;
        scan::record(&transaction, &report.completed_inputs)?;
        let count = scan::count(&transaction)?;
        if count > 0 {
            next.analyzed_photos = count;
        }
        write_snapshot_in(&transaction, &next)?;
        history::replace(&transaction, &rebased)?;
        transaction.commit()?;
        project_snapshot(next, history::available(&state.connection)?)
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
            manually_curated: true,
            occurrences: merged_occurrences,
        });
        normalize_group_order(&mut snapshot.groups);

        let previous =
            read_snapshot(&state.connection)?.expect("the people snapshot was read before merging");
        publish_correction(&mut state.connection, &previous, &snapshot)?;
        project_snapshot(snapshot, true)
    }

    pub(crate) fn split_person(
        &self,
        person_id: &str,
        photo_ids: &[String],
    ) -> AnyResult<PeopleLibrarySnapshot> {
        let mut state = self.lock()?;
        let mut snapshot =
            read_snapshot(&state.connection)?.ok_or_else(|| anyhow!("no people data"))?;
        let before = snapshot.clone();
        let group = snapshot
            .groups
            .iter_mut()
            .find(|g| g.person_id == person_id)
            .ok_or_else(|| anyhow!("person no longer exists"))?;
        let selected = photo_ids.iter().collect::<BTreeSet<_>>();
        let moved = group
            .occurrences
            .iter()
            .filter(|f| selected.contains(&f.photo_id))
            .cloned()
            .collect::<Vec<_>>();
        if moved.is_empty() || moved.len() == group.occurrences.len() {
            bail!("select some, but not all, of this person's photos");
        }
        group
            .occurrences
            .retain(|f| !selected.contains(&f.photo_id));
        group.thumbnail_jpeg.clear();
        group.manually_curated = true;
        snapshot.groups.push(StoredGroup {
            person_id: person_id_for_occurrences(&moved),
            sort_index: u32::MAX,
            display_name: String::new(),
            thumbnail_jpeg: Vec::new(),
            manually_merged: false,
            manually_curated: true,
            occurrences: moved,
        });
        normalize_group_order(&mut snapshot.groups);
        publish_correction(&mut state.connection, &before, &snapshot)?;
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
            return project_snapshot(snapshot, history::available(&state.connection)?);
        }
        let previous = read_snapshot(&state.connection)?.expect("snapshot exists");
        group.display_name = display_name;
        publish_correction(&mut state.connection, &previous, &snapshot)?;
        project_snapshot(snapshot, true)
    }

    pub(crate) fn undo_merge(&self) -> AnyResult<PeopleLibrarySnapshot> {
        let mut state = self.lock()?;
        let (id, previous) = history::latest(&state.connection)?
            .ok_or_else(|| anyhow!("there is no people correction to undo"))?;
        let transaction = state.connection.transaction()?;
        write_snapshot_in(&transaction, &previous)?;
        history::remove(&transaction, id)?;
        transaction.commit()?;
        project_snapshot(previous, history::available(&state.connection)?)
    }

    pub(crate) fn clear(&self) -> AnyResult<()> {
        let mut state = self.lock()?;
        let transaction = state
            .connection
            .transaction()
            .context("begin local people-data clear")?;
        clear_snapshot_tables(&transaction)?;
        history::clear(&transaction)?;
        scan::clear(&transaction)?;
        transaction
            .commit()
            .context("commit local people-data clear")?;
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
    history::initialize(connection)?;
    match stored {
        Some((SCHEMA_VERSION, identity)) if identity == SCHEMA_IDENTITY => {}
        previous => {
            let legacy = match &previous {
                None => false,
                Some((LEGACY_SCHEMA_VERSION, identity)) if identity == LEGACY_SCHEMA_IDENTITY => {
                    true
                }
                Some((2, identity)) if identity == "shadow-people-store-20260831.2" => false,
                Some((3, identity)) if identity == "shadow-people-store-20260922.3" => false,
                Some((version, identity)) => {
                    bail!("unsupported local people store schema {version} ({identity})")
                }
            };
            let transaction = connection
                .transaction()
                .context("begin people portrait migration")?;
            if legacy {
                transaction.execute_batch("ALTER TABLE people_groups ADD COLUMN display_name TEXT NOT NULL DEFAULT '' CHECK (length(CAST(display_name AS BLOB)) <= 256);")?;
            }
            portraits::migrate(&transaction)?;
            transaction.execute("DELETE FROM people_schema", [])?;
            transaction.execute(
                "INSERT INTO people_schema(version, identity) VALUES (?1, ?2)",
                params![SCHEMA_VERSION, SCHEMA_IDENTITY],
            )?;
            transaction
                .commit()
                .context("commit people portrait migration")?;
        }
    }
    scan::initialize(connection)?;
    Ok(())
}

fn stored_occurrence(reference: FaceOccurrenceReference) -> StoredOccurrence {
    StoredOccurrence {
        occurrence_id: reference.occurrence_id.to_string(),
        photo_id: reference.photo_id.to_string(),
        representation_id: reference.representation_id.to_string(),
        thumbnail_jpeg: Vec::new(),
        portrait_quality: 0.0,
        quality_rejected: false,
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
            "SELECT person_id, sort_index, display_name, thumbnail_jpeg, manually_merged, manually_curated
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
                manually_curated: row.get(5)?,
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
                    bounds_x, bounds_y, bounds_width, bounds_height, thumbnail_jpeg, portrait_quality, quality_rejected
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
                    thumbnail_jpeg: row.get(8)?,
                    portrait_quality: row.get(9)?,
                    quality_rejected: row.get(10)?,
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

fn publish_correction(
    connection: &mut Connection,
    before: &StoredSnapshot,
    after: &StoredSnapshot,
) -> AnyResult<()> {
    let transaction = connection.transaction()?;
    history::push(&transaction, before)?;
    write_snapshot_in(&transaction, after)?;
    transaction.commit()?;
    Ok(())
}

fn write_snapshot_in(transaction: &Transaction<'_>, snapshot: &StoredSnapshot) -> AnyResult<()> {
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
                     person_id, sort_index, display_name, thumbnail_jpeg, manually_merged, manually_curated
                 ) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                params![
                    group.person_id,
                    group.sort_index,
                    group.display_name,
                    group.thumbnail_jpeg,
                    group.manually_merged,
                    group.manually_curated,
                ],
            )
            .context("write local people group")?;
        for occurrence in &group.occurrences {
            transaction
                .execute(
                    "INSERT INTO people_occurrences (
                         occurrence_id, person_id, photo_id, representation_id,
                         bounds_x, bounds_y, bounds_width, bounds_height, thumbnail_jpeg, portrait_quality, quality_rejected
                     ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)",
                    params![
                        occurrence.occurrence_id,
                        group.person_id,
                        occurrence.photo_id,
                        occurrence.representation_id,
                        occurrence.bounds[0],
                        occurrence.bounds[1],
                        occurrence.bounds[2],
                        occurrence.bounds[3],
                        occurrence.thumbnail_jpeg,
                        occurrence.portrait_quality,
                        occurrence.quality_rejected,
                    ],
                )
                .context("write local people occurrence")?;
        }
    }
    Ok(())
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
        .filter_map(portraits::project)
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

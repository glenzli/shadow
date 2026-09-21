//! Per-occurrence portrait provenance and conservative quality projection.
use super::{StoredGroup, StoredOccurrence};
use anyhow::Result;
use rusqlite::Transaction;
use std::collections::{HashMap, HashSet};

pub(super) fn migrate(transaction: &Transaction<'_>) -> Result<()> {
    transaction.execute_batch(
        "ALTER TABLE people_occurrences ADD COLUMN thumbnail_jpeg BLOB NOT NULL DEFAULT X'';
         ALTER TABLE people_occurrences ADD COLUMN portrait_quality REAL NOT NULL DEFAULT 0;
         ALTER TABLE people_occurrences ADD COLUMN quality_rejected INTEGER NOT NULL DEFAULT 0 CHECK (quality_rejected IN (0, 1));
         ALTER TABLE people_groups ADD COLUMN manually_curated INTEGER NOT NULL DEFAULT 0 CHECK (manually_curated IN (0, 1));",
    )?;
    // Older splits did not carry a curation bit. Recover it from their durable
    // correction history before quality screening can hide an authored group.
    let Some(snapshot) = super::read_snapshot(transaction)? else {
        return Ok(());
    };
    let current: HashMap<_, _> = snapshot
        .groups
        .iter()
        .flat_map(|g| {
            g.occurrences
                .iter()
                .map(move |f| (f.occurrence_id.as_str(), g.person_id.as_str()))
        })
        .collect();
    let mut split_people = HashSet::new();
    for (_, old) in super::history::read_all(transaction)? {
        for group in old.groups {
            let owners: HashSet<_> = group
                .occurrences
                .iter()
                .filter_map(|f| current.get(f.occurrence_id.as_str()).copied())
                .collect();
            if owners.len() > 1 {
                split_people.extend(owners);
            }
        }
    }
    for id in split_people {
        transaction.execute(
            "UPDATE people_groups SET manually_curated = 1 WHERE person_id = ?1",
            [id],
        )?;
    }
    Ok(())
}

pub(super) fn authored(group: &StoredGroup) -> bool {
    group.manually_curated || group.manually_merged || !group.display_name.is_empty()
}

pub(super) fn project(mut group: StoredGroup) -> Option<StoredGroup> {
    let authored = authored(&group);
    if !authored {
        // Keep rejected occurrences in storage for reversibility, but do not
        // manufacture anonymous people from faces we cannot meaningfully review.
        group.occurrences.retain(|face| !face.quality_rejected);
        if group.occurrences.is_empty() {
            return None;
        }
    }
    let best = group
        .occurrences
        .iter()
        .filter(|face| !face.quality_rejected && !face.thumbnail_jpeg.is_empty())
        .max_by(|a, b| {
            a.portrait_quality
                .total_cmp(&b.portrait_quality)
                .then_with(|| b.occurrence_id.cmp(&a.occurrence_id))
        });
    if let Some(face) = best {
        group.thumbnail_jpeg = face.thumbnail_jpeg.clone();
    } else if authored
        && let Some(face) = group
            .occurrences
            .iter()
            .find(|face| !face.thumbnail_jpeg.is_empty())
    {
        group.thumbnail_jpeg = face.thumbnail_jpeg.clone();
    }
    Some(group)
}

pub(super) fn attach(face: &mut StoredOccurrence, preview: &shadow_core::PeopleFacePreview) {
    face.thumbnail_jpeg = preview.thumbnail_jpeg.clone();
    face.portrait_quality = preview.quality;
    face.quality_rejected = false;
}

#[cfg(test)]
mod tests;

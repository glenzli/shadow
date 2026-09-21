//! Additive reconciliation: model refresh never deletes authored organization.
use super::{
    StoredGroup, StoredOccurrence, StoredSnapshot, bounded_u32, normalize_group_order,
    person_id_for_occurrences, portraits, stored_occurrence,
};
use anyhow::Result as AnyResult;
use shadow_core::PeopleAnalysisReport;
use std::collections::{BTreeSet, HashMap, HashSet};

pub(super) fn reconcile_analysis(
    mut existing: StoredSnapshot,
    report: PeopleAnalysisReport,
) -> AnyResult<StoredSnapshot> {
    let skipped = report.skipped;
    existing.analyzed_photos = bounded_u32(report.analyzed_photos, "analyzed photos")?;
    existing.detected_faces = bounded_u32(report.detected_faces, "detected faces")?;
    existing.embedded_faces = bounded_u32(report.embedded_faces, "embedded faces")?;
    existing.skipped_items = bounded_u32(
        skipped.no_current_visual
            + skipped.unsupported_visual
            + skipped.stale_input
            + skipped.low_detection_confidence
            + skipped.ineligible_embedding
            + skipped.low_face_quality,
        "skipped items",
    )?;
    existing.ungrouped_faces = bounded_u32(report.grouping.ungrouped.len(), "ungrouped faces")?;
    existing.truncated = report.truncated;
    existing.grouping_revision = report.grouping.grouping_revision;
    let mut previews = report
        .face_previews
        .into_iter()
        .map(|p| (p.occurrence_id.clone(), p))
        .collect::<HashMap<_, _>>();
    let candidates = report
        .grouping
        .groups
        .into_iter()
        .map(|g| (g.group_id, g.members))
        .chain(
            report
                .grouping
                .ungrouped
                .into_iter()
                .map(|f| (f.occurrence_id.to_string(), vec![f])),
        );
    let index = OccurrenceIndex::new(&existing.groups);
    let mut matched = HashSet::new();
    for (_, members) in candidates {
        let incoming = members
            .into_iter()
            .map(|reference| {
                let mut face = stored_occurrence(reference);
                if let Some(preview) = previews.remove(&face.occurrence_id) {
                    portraits::attach(&mut face, &preview);
                }
                face
            })
            .collect::<Vec<_>>();
        let mut owners = BTreeSet::new();
        let mut new_faces = Vec::new();
        for mut face in incoming {
            if let Some((group, member)) = index.unique_owner(&face, &matched) {
                owners.insert(group);
                matched.insert((group, member));
                // Preserve the correction identity across changes to evidence ids.
                let old = &mut existing.groups[group].occurrences[member];
                if face.thumbnail_jpeg.is_empty() {
                    face.thumbnail_jpeg = old.thumbnail_jpeg.clone();
                    face.portrait_quality = old.portrait_quality;
                }
                *old = face;
            } else {
                new_faces.push(face);
            }
        }
        if new_faces.is_empty() {
            continue;
        }
        if owners.len() == 1 {
            let group = &mut existing.groups[*owners.first().expect("one owner")];
            let mut conflicts = Vec::new();
            for face in new_faces {
                if group
                    .occurrences
                    .iter()
                    .any(|f| f.photo_id == face.photo_id)
                {
                    conflicts.push(face);
                } else {
                    group.occurrences.push(face);
                }
            }
            new_faces = conflicts;
        }
        if !new_faces.is_empty() {
            existing.groups.push(StoredGroup {
                person_id: person_id_for_occurrences(&new_faces),
                sort_index: u32::MAX,
                display_name: String::new(),
                thumbnail_jpeg: Vec::new(),
                manually_merged: false,
                manually_curated: false,
                occurrences: new_faces,
            });
        }
    }
    // Quality rejections never create a person or erase an occurrence. Match
    // only unambiguous prior geometry from a completed photo, exactly once.
    let completed: HashSet<_> = report
        .completed_inputs
        .iter()
        .map(|i| (i.photo_id.as_str(), i.representation_id.as_str()))
        .collect();
    for rejected in report.rejected_faces {
        if !completed.contains(&(
            rejected.photo_id.as_str(),
            rejected.representation_id.as_str(),
        )) {
            continue;
        }
        let face = StoredOccurrence {
            occurrence_id: String::new(),
            photo_id: rejected.photo_id,
            representation_id: rejected.representation_id,
            bounds: rejected.bounds,
            thumbnail_jpeg: Vec::new(),
            portrait_quality: 0.0,
            quality_rejected: true,
        };
        if let Some((g, m)) = index.unique_owner(&face, &matched) {
            matched.insert((g, m));
            existing.groups[g].occurrences[m].quality_rejected = true;
        }
    }
    normalize_group_order(&mut existing.groups);
    Ok(existing)
}

struct OccurrenceIndex {
    exact: HashMap<String, (usize, usize)>,
    by_photo: HashMap<String, Vec<(usize, usize, StoredOccurrence)>>,
}

impl OccurrenceIndex {
    fn new(groups: &[StoredGroup]) -> Self {
        let mut index = Self {
            exact: HashMap::new(),
            by_photo: HashMap::new(),
        };
        for (g, group) in groups.iter().enumerate() {
            for (m, face) in group.occurrences.iter().enumerate() {
                index.exact.insert(face.occurrence_id.clone(), (g, m));
                index
                    .by_photo
                    .entry(face.photo_id.clone())
                    .or_default()
                    .push((g, m, face.clone()));
            }
        }
        index
    }

    fn unique_owner(
        &self,
        incoming: &StoredOccurrence,
        matched: &HashSet<(usize, usize)>,
    ) -> Option<(usize, usize)> {
        if let Some(&owner) = self.exact.get(&incoming.occurrence_id) {
            return (!matched.contains(&owner)).then_some(owner);
        }
        // Match against the immutable pre-refresh geometry, once per stored
        // occurrence. Never chase geometry updated by another incoming face.
        let mut candidates =
            self.by_photo
                .get(&incoming.photo_id)?
                .iter()
                .filter(|(g, m, face)| {
                    !matched.contains(&(*g, *m))
                        && face.representation_id == incoming.representation_id
                        && overlap(face.bounds, incoming.bounds) >= 0.8
                });
        let (g, m, _) = candidates.next()?;
        candidates.next().is_none().then_some((*g, *m))
    }
}
fn overlap(a: [f32; 4], b: [f32; 4]) -> f32 {
    let w = (a[0] + a[2]).min(b[0] + b[2]) - a[0].max(b[0]);
    let h = (a[1] + a[3]).min(b[1] + b[3]) - a[1].max(b[1]);
    let intersection = w.max(0.) * h.max(0.);
    let union = a[2] * a[3] + b[2] * b[3] - intersection;
    if union > 0. { intersection / union } else { 0. }
}

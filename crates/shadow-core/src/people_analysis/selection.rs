//! Metadata-only incremental planning. No image or biometric cache is persisted.
use super::{
    PeopleAnalysisControl, PeopleAnalysisError, REVIEW_PAGE_SIZE, ensure_active, source_revision,
};
use serde::Serialize;
use shadow_catalog::{CatalogHandle, LibraryPhotoFilter, LibraryPhotoOrder};
use shadow_domain::PreviewCodec;
use shadow_domain::RepresentationId;
use std::collections::BTreeMap;
use std::collections::BTreeSet;

#[derive(Debug, Clone, Eq, PartialEq, Serialize)]
pub struct PeopleAnalysisInput {
    pub photo_id: String,
    pub representation_id: String,
    pub source_revision: String,
}
#[derive(Debug, Clone, Default)]
pub struct PeopleAnalysisSelection {
    pub known_inputs: BTreeMap<String, String>,
    pub anchor_photo_ids: BTreeSet<String>,
}

pub(super) fn select_inputs(
    catalog: &CatalogHandle,
    state: &PeopleAnalysisSelection,
    maximum: usize,
    library: &BTreeMap<String, RepresentationId>,
    control: &(impl PeopleAnalysisControl + ?Sized),
) -> Result<(BTreeSet<RepresentationId>, bool), PeopleAnalysisError> {
    let anchor_budget = maximum.saturating_sub(1).min(64);
    let mut anchors = BTreeSet::new();
    let mut pending = Vec::new();
    let mut more = false;
    let mut cursor = None;
    loop {
        ensure_active(control)?;
        let page = catalog.review_page(cursor.as_ref(), REVIEW_PAGE_SIZE)?;
        for item in page.items {
            if library.get(&item.photo_id.to_string()) != Some(&item.representation_id) {
                continue;
            }
            let Some(record) = item.visual else {
                continue;
            };
            if record.artifact.codec != PreviewCodec::Jpeg {
                continue;
            }
            let revision = format!(
                "{}/input:{}",
                source_revision(item.photo_id, &record),
                super::quality::ANALYSIS_REVISION
            );
            if state.known_inputs.get(&item.representation_id.to_string()) == Some(&revision) {
                if anchors.len() < anchor_budget
                    && state.anchor_photo_ids.contains(&item.photo_id.to_string())
                {
                    anchors.insert(item.representation_id);
                }
            } else if pending.len() < maximum {
                pending.push(item.representation_id);
            } else {
                more = true;
            }
        }
        cursor = page.next_cursor;
        if cursor.is_none() {
            break;
        }
    }
    if pending.is_empty() {
        return Ok((BTreeSet::new(), false));
    }
    let capacity = maximum - anchors.len();
    more |= pending.len() > capacity;
    anchors.extend(pending.into_iter().take(capacity));
    Ok((anchors, more))
}

/// Current photo-first Library membership, including its enabled-source boundary.
/// Reads metadata only; removed sources and unowned legacy paths stay excluded.
pub fn people_analysis_library_membership(
    catalog: &CatalogHandle,
) -> Result<BTreeMap<String, RepresentationId>, PeopleAnalysisError> {
    library_membership(catalog, &super::UnobservedPeopleAnalysis)
}

pub(super) fn library_membership(
    catalog: &CatalogHandle,
    control: &(impl PeopleAnalysisControl + ?Sized),
) -> Result<BTreeMap<String, RepresentationId>, PeopleAnalysisError> {
    let mut members = BTreeMap::new();
    let mut cursor = None;
    loop {
        ensure_active(control)?;
        let page = catalog.library_photo_page(
            &LibraryPhotoFilter::default(),
            LibraryPhotoOrder::default(),
            cursor.as_ref(),
            REVIEW_PAGE_SIZE,
        )?;
        members.extend(
            page.items
                .into_iter()
                .map(|photo| (photo.photo_id.to_string(), photo.representation_id)),
        );
        cursor = page.next_cursor;
        if cursor.is_none() {
            break;
        }
    }
    Ok(members)
}

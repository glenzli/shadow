//! Desktop-session delegation for transient anonymous-person analysis.

use std::path::Path;

use anyhow::{Context, Result as AnyResult};
use shadow_ai::InferRuntimeClient;
use shadow_core::{PeopleAnalysisPolicy, PeopleAnalysisReport, analyze_review_people};

use super::{DesktopSession, ffi};

impl DesktopSession {
    pub(crate) fn analyze_people(
        &self,
        infer_base_url: &str,
        credential_file: &str,
    ) -> AnyResult<ffi::FfiPeopleAnalysisReport> {
        let provider =
            InferRuntimeClient::from_credential_file(infer_base_url, Path::new(credential_file))
                .context("configure local people-analysis provider")?;
        let report = analyze_review_people(
            &self.catalog,
            &self.cache_root,
            &provider,
            PeopleAnalysisPolicy::default(),
        )
        .context("analyze current Library visuals for anonymous people")?;
        ffi_people_analysis_report(report)
    }
}

fn ffi_people_analysis_report(
    report: PeopleAnalysisReport,
) -> AnyResult<ffi::FfiPeopleAnalysisReport> {
    let skipped_items = report
        .skipped
        .no_current_visual
        .saturating_add(report.skipped.unsupported_visual)
        .saturating_add(report.skipped.stale_input)
        .saturating_add(report.skipped.low_detection_confidence)
        .saturating_add(report.skipped.ineligible_embedding);
    Ok(ffi::FfiPeopleAnalysisReport {
        analyzed_photos: bounded_u32(report.analyzed_photos, "analyzed photo count")?,
        detected_faces: bounded_u32(report.detected_faces, "detected face count")?,
        embedded_faces: bounded_u32(report.embedded_faces, "embedded face count")?,
        skipped_items: bounded_u32(skipped_items, "skipped item count")?,
        ungrouped_faces: bounded_u32(report.grouping.ungrouped.len(), "ungrouped face count")?,
        truncated: report.truncated,
        groups: report
            .grouping
            .groups
            .into_iter()
            .map(|group| {
                Ok(ffi::FfiPeopleGroup {
                    group_id: group.group_id,
                    member_count: bounded_u32(group.members.len(), "people group member count")?,
                })
            })
            .collect::<AnyResult<Vec<_>>>()?,
    })
}

fn bounded_u32(value: usize, field: &'static str) -> AnyResult<u32> {
    u32::try_from(value).with_context(|| format!("{field} exceeds the desktop ABI bound"))
}

#[cfg(test)]
mod tests;

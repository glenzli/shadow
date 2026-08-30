//! Desktop-session delegation for transient anonymous-person analysis.

use std::{collections::HashMap, path::Path};

use anyhow::{Context, Result as AnyResult};
use shadow_ai::InferRuntimeClient;
use shadow_core::{
    PeopleAnalysisPolicy, PeopleAnalysisReport, analyze_review_people,
    analyze_review_people_with_control,
};

use super::{
    DesktopSession, ffi,
    people_analysis_service::{PeopleAnalysisJobOutcome, PeopleAnalysisJobSnapshot},
};

impl DesktopSession {
    pub(crate) fn analyze_people(
        &self,
        infer_base_url: &str,
        credential_file: &str,
    ) -> AnyResult<ffi::FfiPeopleAnalysisReport> {
        let provider = InferRuntimeClient::from_credential_file_with_discovery(
            (!infer_base_url.is_empty()).then_some(infer_base_url),
            Path::new(credential_file),
        )
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

    pub(crate) fn begin_people_analysis_job(&self) -> AnyResult<u64> {
        Ok(self
            .people_analyses
            .begin_job(PeopleAnalysisPolicy::default().maximum_photos)?)
    }

    pub(crate) fn people_analysis_job_status(
        &self,
        job_token: u64,
    ) -> AnyResult<ffi::FfiPeopleAnalysisJobStatus> {
        ffi_people_analysis_job_status(self.people_analyses.snapshot(job_token)?)
    }

    pub(crate) fn cancel_people_analysis_job(&self, job_token: u64) -> AnyResult<bool> {
        Ok(self.people_analyses.cancel_job(job_token)?)
    }

    pub(crate) fn execute_people_analysis_job(
        &self,
        job_token: u64,
        infer_base_url: &str,
        credential_file: &str,
    ) -> AnyResult<ffi::FfiPeopleAnalysisExecution> {
        let policy = PeopleAnalysisPolicy::default();
        let outcome = self.people_analyses.execute_job(job_token, |control| {
            let provider = InferRuntimeClient::from_credential_file_with_discovery(
                (!infer_base_url.is_empty()).then_some(infer_base_url),
                Path::new(credential_file),
            )
            .context("configure local people-analysis provider")?;
            analyze_review_people_with_control(
                &self.catalog,
                &self.cache_root,
                &provider,
                policy,
                control,
            )
            .context("analyze current Library visuals for anonymous people")
        })?;
        match outcome {
            PeopleAnalysisJobOutcome::Ready(report) => Ok(ffi::FfiPeopleAnalysisExecution {
                job_token,
                cancelled: false,
                diagnostic: String::new(),
                report: ffi_people_analysis_report(report)?,
            }),
            PeopleAnalysisJobOutcome::Cancelled => Ok(ffi::FfiPeopleAnalysisExecution {
                job_token,
                cancelled: true,
                diagnostic: String::new(),
                report: empty_people_analysis_report(),
            }),
            PeopleAnalysisJobOutcome::Failed(diagnostic) => Ok(ffi::FfiPeopleAnalysisExecution {
                job_token,
                cancelled: false,
                diagnostic,
                report: empty_people_analysis_report(),
            }),
        }
    }

    pub(crate) fn retire_people_analysis_job(&self, job_token: u64) -> AnyResult<()> {
        Ok(self.people_analyses.retire_job(job_token)?)
    }
}

fn ffi_people_analysis_job_status(
    snapshot: PeopleAnalysisJobSnapshot,
) -> AnyResult<ffi::FfiPeopleAnalysisJobStatus> {
    Ok(ffi::FfiPeopleAnalysisJobStatus {
        job_token: snapshot.token,
        phase: snapshot.phase.code().into(),
        analyzed_photos: bounded_u32(snapshot.analyzed_photos, "analyzed photo progress")?,
        maximum_photos: bounded_u32(snapshot.maximum_photos, "maximum photo progress")?,
        detected_faces: bounded_u32(snapshot.detected_faces, "detected face progress")?,
        compared_faces: bounded_u32(snapshot.compared_faces, "compared face progress")?,
        cancellation_requested: snapshot.cancellation_requested,
        terminal: snapshot.phase.is_terminal(),
    })
}

fn empty_people_analysis_report() -> ffi::FfiPeopleAnalysisReport {
    ffi::FfiPeopleAnalysisReport {
        analyzed_photos: 0,
        detected_faces: 0,
        embedded_faces: 0,
        skipped_items: 0,
        ungrouped_faces: 0,
        truncated: false,
        groups: Vec::new(),
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
    let mut group_previews = report
        .group_previews
        .into_iter()
        .map(|preview| (preview.group_id, preview.thumbnail_jpeg))
        .collect::<HashMap<_, _>>();
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
                let member_count = bounded_u32(group.members.len(), "people group member count")?;
                let photo_ids = group
                    .members
                    .into_iter()
                    .map(|member| member.photo_id.to_string())
                    .collect();
                let thumbnail_jpeg = group_previews.remove(&group.group_id).unwrap_or_default();
                Ok(ffi::FfiPeopleGroup {
                    group_id: group.group_id,
                    member_count,
                    photo_ids,
                    thumbnail_jpeg,
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

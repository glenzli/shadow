//! Desktop-session delegation for authorized local people organization.

use std::path::Path;

use anyhow::{Context, Result as AnyResult, bail};
use shadow_ai::InferRuntimeClient;
use shadow_core::{PeopleAnalysisPolicy, analyze_review_people_incremental};

use super::{
    DesktopSession, ffi,
    people_analysis_service::{PeopleAnalysisJobOutcome, PeopleAnalysisJobSnapshot},
    people_library_store::PeopleLibrarySnapshot,
};

impl DesktopSession {
    pub(crate) fn people_library_snapshot(&self) -> AnyResult<ffi::FfiPeopleAnalysisReport> {
        let members = shadow_core::people_analysis_library_membership(&self.catalog)?;
        ffi_people_analysis_report(
            self.people_library
                .snapshot_for_library(&members.into_keys().collect())?,
        )
    }

    pub(crate) fn begin_people_analysis_job(&self, authorized: bool) -> AnyResult<u64> {
        if !authorized {
            bail!("local people analysis requires explicit user authorization");
        }
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
        authorized: bool,
    ) -> AnyResult<ffi::FfiPeopleAnalysisExecution> {
        if !authorized {
            let _ = self.people_analyses.cancel_job(job_token)?;
        }
        let policy = PeopleAnalysisPolicy::default();
        let selection = self.people_library.analysis_selection()?;
        let outcome = self.people_analyses.execute_job(job_token, |control| {
            let provider = InferRuntimeClient::from_credential_file_with_discovery(
                (!infer_base_url.is_empty()).then_some(infer_base_url),
                Path::new(credential_file),
            )
            .context("configure local people-analysis provider")?;
            analyze_review_people_incremental(
                &self.catalog,
                &self.cache_root,
                &provider,
                policy,
                control,
                Some(&selection),
            )
            .context("analyze current Library visuals for anonymous people")
        })?;
        match outcome {
            PeopleAnalysisJobOutcome::Ready(report) => {
                let made_progress = report.completed_inputs.iter().any(|input| {
                    selection.known_inputs.get(&input.representation_id)
                        != Some(&input.source_revision)
                });
                let snapshot = self
                    .people_library
                    .replace_analysis(report)
                    .context("publish local people organization")?;
                Ok(ffi::FfiPeopleAnalysisExecution {
                    made_progress,
                    job_token,
                    cancelled: false,
                    diagnostic: String::new(),
                    report: {
                        let _ = snapshot;
                        self.people_library_snapshot()?
                    },
                })
            }
            PeopleAnalysisJobOutcome::Cancelled => Ok(ffi::FfiPeopleAnalysisExecution {
                made_progress: false,
                job_token,
                cancelled: true,
                diagnostic: String::new(),
                report: empty_people_analysis_report(),
            }),
            PeopleAnalysisJobOutcome::Failed(diagnostic) => Ok(ffi::FfiPeopleAnalysisExecution {
                made_progress: false,
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

    pub(crate) fn merge_people(
        &self,
        person_ids: Vec<String>,
    ) -> AnyResult<ffi::FfiPeopleAnalysisReport> {
        self.people_library.merge_people(&person_ids)?;
        self.people_library_snapshot()
    }

    pub(crate) fn split_person(
        &self,
        person_id: &str,
        photo_ids: Vec<String>,
    ) -> AnyResult<ffi::FfiPeopleAnalysisReport> {
        self.people_library.split_person(person_id, &photo_ids)?;
        self.people_library_snapshot()
    }

    pub(crate) fn undo_people_merge(&self) -> AnyResult<ffi::FfiPeopleAnalysisReport> {
        self.people_library.undo_merge()?;
        self.people_library_snapshot()
    }

    pub(crate) fn rename_person(
        &self,
        person_id: &str,
        display_name: &str,
    ) -> AnyResult<ffi::FfiPeopleAnalysisReport> {
        self.people_library.rename_person(person_id, display_name)?;
        self.people_library_snapshot()
    }

    pub(crate) fn reset_people_analysis_progress(&self) -> AnyResult<()> {
        self.people_library.reset_analysis_progress()
    }

    pub(crate) fn clear_people_data(&self) -> AnyResult<()> {
        self.people_library.clear()
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
        has_data: false,
        analyzed_photos: 0,
        detected_faces: 0,
        embedded_faces: 0,
        skipped_items: 0,
        ungrouped_faces: 0,
        truncated: false,
        groups: Vec::new(),
        can_undo_merge: false,
    }
}

fn ffi_people_analysis_report(
    snapshot: PeopleLibrarySnapshot,
) -> AnyResult<ffi::FfiPeopleAnalysisReport> {
    Ok(ffi::FfiPeopleAnalysisReport {
        has_data: snapshot.has_data,
        analyzed_photos: snapshot.analyzed_photos,
        detected_faces: snapshot.detected_faces,
        embedded_faces: snapshot.embedded_faces,
        skipped_items: snapshot.skipped_items,
        ungrouped_faces: snapshot.ungrouped_faces,
        truncated: snapshot.truncated,
        groups: snapshot
            .groups
            .into_iter()
            .map(|group| {
                Ok(ffi::FfiPeopleGroup {
                    group_id: group.person_id,
                    display_name: group.display_name,
                    member_count: group.member_count,
                    photo_ids: group.photo_ids,
                    thumbnail_jpeg: group.thumbnail_jpeg,
                    manually_merged: group.manually_merged,
                })
            })
            .collect::<AnyResult<Vec<_>>>()?,
        can_undo_merge: snapshot.can_undo_merge,
    })
}

fn bounded_u32(value: usize, field: &'static str) -> AnyResult<u32> {
    u32::try_from(value).with_context(|| format!("{field} exceeds the desktop ABI bound"))
}

#[cfg(test)]
mod tests;

use std::path::PathBuf;

use shadow_ai::{
    ArtifactHashAlgorithm, GeneratedArtifactReference, RAW_FOUNDATION_ENCODING_VERSION,
    RAW_FOUNDATION_MEDIA_TYPE, RasterExtent, RawFoundationArtifact, RawFoundationProvenance,
    RawFoundationSourceProvenance,
};
use shadow_catalog::RepresentationFingerprint;

use super::*;

fn invocation(request_id: &str, generation: u64) -> RawFoundationInvocation {
    RawFoundationInvocation {
        request_id: request_id.into(),
        generation,
        photo_id: "018f3ec1-6219-7df2-a52d-f744c4f88533".into(),
        input_raw: PathBuf::from("/source.CR2"),
    }
}

fn ready(source_path: &str, modified_at_ms: i64) -> RawFoundationReady {
    RawFoundationReady {
        descriptor: RawFoundationArtifact::new(
            GeneratedArtifactReference::new(
                ArtifactHashAlgorithm::Sha256,
                "1".repeat(64),
                4_096,
                RAW_FOUNDATION_MEDIA_TYPE.into(),
                RAW_FOUNDATION_ENCODING_VERSION,
            )
            .expect("artifact reference"),
            RasterExtent::new(6, 4).expect("extent"),
            RawFoundationSourceProvenance::new("2".repeat(64), 8_192, "3".repeat(64))
                .expect("source provenance"),
            RawFoundationProvenance::new(
                "4".repeat(64),
                "5".repeat(64),
                "6".repeat(64),
                "7".repeat(64),
                shadow_bridge::RAW_FOUNDATION_IMPLEMENTATION_REVISION.into(),
            )
            .expect("foundation provenance"),
        )
        .expect("foundation descriptor"),
        path: PathBuf::from(format!("{source_path}.shadowrawf")),
        source_path: PathBuf::from(source_path),
        source: RepresentationFingerprint {
            byte_len: 8_192,
            modified_at_ms: Some(modified_at_ms),
        },
        disposition: RawFoundationMaterializationDisposition::Published,
    }
}

#[test]
fn progress_and_unavailable_terminal_remain_pollable_until_retired() {
    let service = RawFoundationService::new();
    let token = service.begin_job("request-1".into(), 4).expect("begin job");

    let terminal = service
        .execute_job_with(
            token,
            invocation("request-1", 4),
            |_invocation, _cancellation, progress| {
                progress.publish(RuntimeProgress {
                    phase_code: "inference".into(),
                    completed_basis_points: 2_500,
                });
                Ok::<_, &'static str>(RawFoundationRuntimeOutcome::Unavailable {
                    diagnostic: "model_not_installed".into(),
                })
            },
        )
        .expect("execute job");

    assert_eq!(terminal.phase, RawFoundationJobPhase::Unavailable);
    assert_eq!(terminal.completed_basis_points, 2_500);
    assert_eq!(terminal.diagnostic, "model_not_installed");
    assert_eq!(service.snapshot(token).expect("poll terminal"), terminal);
    assert!(
        service
            .ready_foundation(token)
            .expect("ready query")
            .is_none()
    );
    service.retire_job(token).expect("retire terminal");
    assert!(matches!(
        service.snapshot(token),
        Err(RawFoundationServiceError::UnknownJob(value)) if value == token
    ));
}

#[test]
fn cancellation_wins_over_a_racing_runtime_terminal() {
    let service = RawFoundationService::new();
    let token = service.begin_job("request-2".into(), 9).expect("begin job");

    let terminal = service
        .execute_job_with(
            token,
            invocation("request-2", 9),
            |_invocation, cancellation, _progress| {
                cancellation.cancel();
                Ok::<_, &'static str>(RawFoundationRuntimeOutcome::Failed {
                    diagnostic: "late provider failure".into(),
                })
            },
        )
        .expect("execute cancelled job");

    assert_eq!(terminal.phase, RawFoundationJobPhase::Cancelled);
    assert!(terminal.cancellation_requested);
    assert!(terminal.diagnostic.is_empty());
}

#[test]
fn invocation_identity_mismatch_does_not_consume_the_job() {
    let service = RawFoundationService::new();
    let token = service.begin_job("request-3".into(), 2).expect("begin job");

    assert!(matches!(
        service.execute_job_with(
            token,
            invocation("substituted", 2),
            |_invocation, _cancellation, _progress| {
                Ok::<_, &'static str>(RawFoundationRuntimeOutcome::Cancelled)
            },
        ),
        Err(RawFoundationServiceError::InvocationIdentityMismatch)
    ));
    assert_eq!(
        service.snapshot(token).expect("job remains queued").phase,
        RawFoundationJobPhase::Queued
    );
}

#[test]
fn preflight_failure_is_pollable_and_retireable_without_runtime_execution() {
    let service = RawFoundationService::new();
    let token = service
        .begin_job("preflight-failure".into(), 7)
        .expect("begin job");

    let terminal = service
        .complete_preflight_failure(token, "source identity mismatch".into())
        .expect("complete preflight failure");
    assert_eq!(terminal.phase, RawFoundationJobPhase::Failed);
    assert_eq!(terminal.phase_code, "failed");
    assert_eq!(terminal.diagnostic, "source identity mismatch");
    assert_eq!(service.snapshot(token).expect("poll failure"), terminal);
    service.retire_job(token).expect("retire preflight failure");
    assert!(matches!(
        service.snapshot(token),
        Err(RawFoundationServiceError::UnknownJob(value)) if value == token
    ));
}

#[test]
fn cancellation_wins_over_a_preflight_failure() {
    let service = RawFoundationService::new();
    let token = service
        .begin_job("cancelled-preflight".into(), 8)
        .expect("begin job");
    service.cancel_job(token).expect("request cancellation");

    let terminal = service
        .complete_preflight_failure(token, "late source error".into())
        .expect("complete cancelled preflight");
    assert_eq!(terminal.phase, RawFoundationJobPhase::Cancelled);
    assert!(terminal.cancellation_requested);
    assert!(terminal.diagnostic.is_empty());
    service
        .retire_job(token)
        .expect("retire cancelled preflight");
}

#[test]
fn ready_result_remains_resolvable_by_exact_source_after_job_retirement() {
    let service = RawFoundationService::new();
    let token = service.begin_job("request-4".into(), 1).expect("begin job");
    let published = ready("/source/ready.nef", 41);

    let terminal = service
        .execute_job_with(
            token,
            invocation("request-4", 1),
            |_invocation, _cancellation, _progress| {
                Ok::<_, &'static str>(RawFoundationRuntimeOutcome::Ready(Box::new(
                    published.clone(),
                )))
            },
        )
        .expect("execute ready job");
    assert_eq!(terminal.phase, RawFoundationJobPhase::Ready);
    service.retire_job(token).expect("retire ready job");

    assert_eq!(
        service
            .ready_for_source(&published.source_path, published.source)
            .expect("resolve ready source"),
        Some(published.clone())
    );
    assert!(
        service
            .ready_for_source(
                &published.source_path,
                RepresentationFingerprint {
                    byte_len: published.source.byte_len,
                    modified_at_ms: Some(42),
                },
            )
            .expect("resolve changed source")
            .is_none()
    );
    assert!(
        service
            .ready_for_source(std::path::Path::new("/source/other.nef"), published.source,)
            .expect("resolve different path")
            .is_none()
    );
}

#[test]
fn ready_registry_is_bounded_and_prefers_the_newest_exact_source() {
    let service = RawFoundationService::new();
    for index in 0..=MAX_READY_RAW_FOUNDATIONS {
        service
            .remember_ready(ready(
                &format!("/source/{index}.nef"),
                i64::try_from(index).expect("bounded fixture index"),
            ))
            .expect("publish ready fixture");
    }

    assert_eq!(
        service
            .ready_foundations_guard()
            .expect("ready registry")
            .len(),
        MAX_READY_RAW_FOUNDATIONS
    );
    assert!(
        service
            .ready_for_source(
                std::path::Path::new("/source/0.nef"),
                RepresentationFingerprint {
                    byte_len: 8_192,
                    modified_at_ms: Some(0),
                },
            )
            .expect("oldest lookup")
            .is_none()
    );

    let replacement = ready("/source/16.nef", 16);
    service
        .remember_ready(replacement.clone())
        .expect("replace newest");
    assert_eq!(
        service
            .ready_for_source(&replacement.source_path, replacement.source)
            .expect("newest lookup"),
        Some(replacement)
    );
    assert_eq!(
        service
            .ready_foundations_guard()
            .expect("ready registry")
            .len(),
        MAX_READY_RAW_FOUNDATIONS
    );
}

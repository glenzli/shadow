use super::super::super::{contract::DecodeInspectionRequest, submission::InspectionMessage};
use super::*;
use crate::ScanCancellation;
use shadow_catalog::RepresentationFingerprint;
use shadow_domain::EntityId;
use std::sync::mpsc;

fn job(id: RepresentationId) -> RefinementJob {
    RefinementJob {
        inspection: InspectionMessage {
            request: DecodeInspectionRequest {
                representation_id: id,
                path: "fixture.dng".into(),
                expected_source: RepresentationFingerprint {
                    byte_len: 1,
                    modified_at_ms: Some(1),
                },
            },
            cancellation: ScanCancellation::new(),
            response: mpsc::channel().0,
            enqueued_at: None,
        },
        provider_id: "test".into(),
        provider_version: "1".into(),
        fallback: None,
    }
}

fn pop_id(queue: &RefinementQueue) -> RepresentationId {
    queue.pop().unwrap().inspection.request.representation_id
}

#[test]
fn selected_then_visible_then_fifo_preserves_every_job() {
    let ids = (0..5)
        .map(|_| RepresentationId::new_v7())
        .collect::<Vec<_>>();
    let priority = RefinementPriority::default();
    let queue = RefinementQueue::new(priority.clone());
    for id in &ids {
        assert!(queue.push(job(*id)).is_ok());
    }
    priority.replace(Some(ids[4]), &[ids[2], ids[3]]);
    queue.close();
    assert_eq!(
        (0..5).map(|_| pop_id(&queue)).collect::<Vec<_>>(),
        vec![ids[4], ids[2], ids[3], ids[0], ids[1]]
    );
    assert!(queue.pop().is_none());
}

#[test]
fn replaced_viewport_applies_to_queued_and_late_arriving_jobs() {
    let a = RepresentationId::new_v7();
    let b = RepresentationId::new_v7();
    let c = RepresentationId::new_v7();
    let priority = RefinementPriority::default();
    let queue = RefinementQueue::new(priority.clone());
    priority.replace(None, &[a]);
    assert!(queue.push(job(a)).is_ok());
    assert!(queue.push(job(b)).is_ok());
    priority.replace(None, &[c]);
    assert!(queue.push(job(c)).is_ok());
    assert_eq!(pop_id(&queue), c);
    priority.replace(None, &[]);
    assert_eq!(pop_id(&queue), a);
    assert_eq!(pop_id(&queue), b);
}

#[test]
fn continuous_foreground_work_cannot_starve_oldest_background_job() {
    let priority = RefinementPriority::default();
    let queue = RefinementQueue::new(priority.clone());
    let background = RepresentationId::new_v7();
    assert!(queue.push(job(background)).is_ok());
    for _ in 0..PRIORITY_BURST {
        let visible = RepresentationId::new_v7();
        priority.replace(Some(visible), &[visible]);
        assert!(queue.push(job(visible)).is_ok());
        assert_eq!(pop_id(&queue), visible);
    }
    let visible = RepresentationId::new_v7();
    priority.replace(Some(visible), &[visible]);
    assert!(queue.push(job(visible)).is_ok());
    assert_eq!(pop_id(&queue), background);
    assert_eq!(pop_id(&queue), visible);
}

#[test]
fn hints_are_bounded_and_cancelled_admission_keeps_its_completion_owner() {
    let priority = RefinementPriority::default();
    let ids = (0..MAX_VISIBLE_REFINEMENTS + 1)
        .map(|_| RepresentationId::new_v7())
        .collect::<Vec<_>>();
    priority.replace(None, &ids);
    assert_eq!(
        priority.0.lock().unwrap().representations.len(),
        MAX_VISIBLE_REFINEMENTS
    );
    let queue = RefinementQueue::new(priority);
    let pending = job(ids[0]);
    pending.inspection.cancellation.cancel();
    let returned = queue.push(pending).err().unwrap();
    assert_eq!(returned.inspection.request.representation_id, ids[0]);
    queue.close();
    assert!(queue.pop().is_none());
}

//! Actor-side execution for durable export-queue messages.

use crate::Catalog;

use super::super::protocol::ExportQueueMessage;

pub(super) fn run_export_queue_message(catalog: &mut Catalog, message: ExportQueueMessage) {
    match message {
        ExportQueueMessage::Enqueue(request, response) => {
            let _ = response.send(catalog.enqueue_export_job(request.as_ref()));
        }
        ExportQueueMessage::Job(job_id, response) => {
            let _ = response.send(catalog.export_job(job_id));
        }
        ExportQueueMessage::Jobs(limit, response) => {
            let _ = response.send(catalog.export_jobs(limit));
        }
        ExportQueueMessage::JobItems(job_id, response) => {
            let _ = response.send(catalog.export_job_items(job_id));
        }
        ExportQueueMessage::Item(item_id, response) => {
            let _ = response.send(catalog.export_item(item_id));
        }
        ExportQueueMessage::JobProgress(job_id, response) => {
            let _ = response.send(catalog.export_job_progress(job_id));
        }
        ExportQueueMessage::ClaimNext(now_ms, response) => {
            let _ = response.send(catalog.claim_next_export_item(now_ms));
        }
        ExportQueueMessage::Advance(request, response) => {
            let _ = response.send(catalog.advance_export_item(request.as_ref()));
        }
        ExportQueueMessage::OutputReceipt(item_id, response) => {
            let _ = response.send(catalog.export_output_receipt(item_id));
        }
        ExportQueueMessage::Cancel(job_id, now_ms, response) => {
            let _ = response.send(catalog.cancel_export_job(job_id, now_ms));
        }
        ExportQueueMessage::RecoverInterrupted(now_ms, response) => {
            let _ = response.send(catalog.recover_interrupted_export_jobs(now_ms));
        }
        ExportQueueMessage::RecoverAndRequeue(now_ms, response) => {
            let _ = response.send(catalog.recover_and_requeue_interrupted_export_items(now_ms));
        }
    }
}

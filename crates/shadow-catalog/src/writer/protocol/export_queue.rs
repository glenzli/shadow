//! Actor messages for durable export jobs, worker transitions, and recovery.

use std::sync::mpsc::SyncSender;

use crate::{
    AdvanceExportItem, CatalogError, EnqueueExportJob, ExportItemId, ExportItemRecord, ExportJobId,
    ExportJobProgress, ExportJobRecord, ExportOutputReceiptRecord, ExportQueueRecovery,
};

pub(in crate::writer) enum ExportQueueMessage {
    Enqueue(
        Box<EnqueueExportJob>,
        SyncSender<Result<ExportJobRecord, CatalogError>>,
    ),
    Job(
        ExportJobId,
        SyncSender<Result<Option<ExportJobRecord>, CatalogError>>,
    ),
    Jobs(
        usize,
        SyncSender<Result<Vec<ExportJobRecord>, CatalogError>>,
    ),
    JobItems(
        ExportJobId,
        SyncSender<Result<Vec<ExportItemRecord>, CatalogError>>,
    ),
    Item(
        ExportItemId,
        SyncSender<Result<Option<ExportItemRecord>, CatalogError>>,
    ),
    JobProgress(
        ExportJobId,
        SyncSender<Result<ExportJobProgress, CatalogError>>,
    ),
    ClaimNext(
        i64,
        SyncSender<Result<Option<ExportItemRecord>, CatalogError>>,
    ),
    Advance(
        Box<AdvanceExportItem>,
        SyncSender<Result<ExportItemRecord, CatalogError>>,
    ),
    OutputReceipt(
        ExportItemId,
        SyncSender<Result<Option<ExportOutputReceiptRecord>, CatalogError>>,
    ),
    Cancel(
        ExportJobId,
        i64,
        SyncSender<Result<ExportJobRecord, CatalogError>>,
    ),
    RecoverInterrupted(i64, SyncSender<Result<usize, CatalogError>>),
    RecoverAndRequeue(i64, SyncSender<Result<ExportQueueRecovery, CatalogError>>),
}

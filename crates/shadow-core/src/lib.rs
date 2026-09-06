//! Application use cases shared by the desktop app and developer CLI.

mod cached_artifact_loader;
mod catalog_backup;
mod decode_inspection;
mod derived_raster_store;
mod derived_raster_workflow;
#[cfg(test)]
mod display_jpeg_fixture;
mod image_understanding;
mod import;
mod library_metadata;
mod native_path;
mod people_analysis;
mod performance;
mod semantic_search;
mod smart_classification;
mod technical_observation;
mod vision_input;
mod work_scheduler;

pub use cached_artifact_loader::{
    CachedArtifactInvalidationReason, CachedArtifactLoadError, CachedArtifactLoader,
};
pub use catalog_backup::{
    CatalogBackupCompletion, CatalogBackupJob, CatalogBackupJobError, CatalogBackupPlan,
    CatalogBackupPolicy, CatalogBackupPolicyError, CatalogBackupRetention,
    CatalogBackupScheduleError, CatalogBackupService,
};
pub use decode_inspection::{
    DecodeInspectionActor, DecodeInspectionDiscardReason, DecodeInspectionError,
    DecodeInspectionHandle, DecodeInspectionOutcome, DecodeInspectionPool,
    DecodeInspectionProgress, DecodeInspectionRequest, DecodeInspectionSummary,
    DecodeInspectionTerminal, DecodeInspectionTicket, DecodeInspector, EmbeddedPreviewPublication,
    EmbeddedPreviewSink, PreviewCacheOutcome, fingerprint_source,
    recommended_decode_inspection_worker_count,
};
pub use derived_raster_store::{
    DerivedRasterStoreError, FilesystemDerivedRasterStore, SHADOW_RGBA8_ENCODING_VERSION,
    SHADOW_RGBA8_MEDIA_TYPE, SHADOW_SOFT_MASK_ENCODING_VERSION, SHADOW_SOFT_MASK_MEDIA_TYPE,
    managed_image_completion_region, managed_soft_mask_definition,
};
pub use derived_raster_workflow::{
    CurrentDerivedRasterPromotionFailure, DerivedRasterStageError, DerivedRasterStageOutcome,
    DerivedRasterStageReceipt, StagedDerivedRasterProposal, execute_and_stage_derived_raster,
    execute_and_stage_derived_raster_with_progress, promote_staged_derived_raster_if_current,
};
pub use image_understanding::{
    AdvancedClassificationReviewError, AdvancedClassificationReviewRequest,
    ClassificationReviewProposal, ClassificationReviewProposalDisposition,
    DEFAULT_IMAGE_UNDERSTANDING_BATCH_SIZE, IMAGE_UNDERSTANDING_SCAN_POLICY_VERSION,
    ImageUnderstandingBatch, ImageUnderstandingKeywordAcceptance,
    ImageUnderstandingKeywordApplication, ImageUnderstandingPolicyError,
    ImageUnderstandingProposal, ImageUnderstandingProposalDisposition, ImageUnderstandingRequest,
    ImageUnderstandingRunSnapshot, ImageUnderstandingRunStatus, ImageUnderstandingScanPolicy,
    ImageUnderstandingScanScope, ImageUnderstandingStoreError, ImageUnderstandingWorkflowError,
    MAX_IMAGE_UNDERSTANDING_BATCH_SIZE, accept_advanced_classification_review,
    advanced_classification_review, apply_image_understanding_keywords,
    dismiss_advanced_classification_review, image_understanding_proposal,
    image_understanding_snapshot, pause_image_understanding, process_image_understanding_batch,
    review_smart_classification_with_model,
};
pub use import::{
    CatalogRelinkConfirmation, ConfirmedRelink, PendingStrongRelink, ProfiledScanReport,
    RelinkApplyError, RelinkCandidate, RelinkIdentityLookup, RelinkSource, RelinkVerificationError,
    SafeReattachPlan, SafeReattachPlanningError, ScanCancellation, ScanCompletion, ScanError,
    ScanIssue, ScanPhase, ScanProgress, ScanReport, SourceRelinkResolution,
    StrongRelinkVerification, UnsupportedRelinkSource, VerifiedRelink, WeakRelinkEvidence,
    WeakRelinkMetadata, apply_confirmed_relink, confirm_verified_relink, discover_source_relink,
    plan_safe_reattach, relink_candidate_from_missing_location, resume_scan,
    resume_scan_controlled, resume_scan_with_inspection, resume_scan_with_inspection_controlled,
    scan_folder, scan_folder_controlled, scan_folder_profiled, scan_folder_profiled_controlled,
    scan_folder_with_inspection, scan_folder_with_inspection_controlled,
    scan_folder_with_inspection_profiled, scan_folder_with_inspection_profiled_controlled,
    verify_pending_relink,
};
pub use library_metadata::{
    GpsMatchPreview, GpsMatchProposal, GpsMatchSettings, GpsPhotoCapture, GpxImportError, GpxTrack,
    GpxTrackPoint, load_gpx_track, match_photos_to_gpx,
};
pub use native_path::{NativePathError, native_location, native_path_from_location};
pub use people_analysis::{
    DEFAULT_PEOPLE_MAXIMUM_COSINE_DISTANCE, PeopleAnalysisControl, PeopleAnalysisError,
    PeopleAnalysisPhase, PeopleAnalysisPolicy, PeopleAnalysisProgress, PeopleAnalysisReport,
    PeopleAnalysisSkipped, PeopleGroupPreview, analyze_review_people,
    analyze_review_people_with_control,
};
pub use performance::{
    DecodePerformance, DurationStats, IMPORT_ENGINE_PERFORMANCE_SCHEMA_VERSION,
    ImportEnginePerformance, ScanPerformance, TechnicalPerformance,
};
pub use semantic_search::{
    DEFAULT_SEMANTIC_SEARCH_MAXIMUM_PHOTOS, MAX_SEMANTIC_SEARCH_PHOTOS, SemanticSearchError,
    SemanticSearchMatch, SemanticSearchPolicy, SemanticSearchReport, SemanticSearchSkipped,
    search_review_semantics, search_review_semantics_with_control,
};
pub use smart_classification::{
    SmartCategoryCount, SmartCategoryDefinition, SmartCategoryFeedbackDecision, SmartCategoryMatch,
    SmartCategoryReviewDecision, SmartCategoryReviewItem, SmartClassificationBatch,
    SmartClassificationError, SmartClassificationPolicy, SmartClassificationRequest,
    SmartClassificationSnapshot, SmartClassificationStatus, classify_review_smart_categories,
    complete_smart_category_review, pause_smart_classification, set_smart_category_feedback,
    smart_category_members, smart_category_review_queue, smart_classification_snapshot,
};
pub use technical_observation::{
    TECHNICAL_ANALYSIS_MAX_EDGE, TechnicalObservationActor, TechnicalObservationError,
    TechnicalObservationHandle, TechnicalObservationOutcome, TechnicalObservationTicket,
    technical_analysis_preprocessing_version,
};
pub use work_scheduler::{
    WorkCancellation, WorkContext, WorkDiscardReason, WorkGeneration, WorkGenerationSource,
    WorkOutcome, WorkPriority, WorkScheduler, WorkSchedulerConfig, WorkSchedulerError,
    WorkSchedulerHandle, WorkSchedulerSnapshot, WorkSpec, WorkSubmitError, WorkTicket,
    WorkWaitError,
};

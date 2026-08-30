#pragma once

#include "backend/cache_types.hpp"
#include "backend/edit_types.hpp"
#include "backend/history_types.hpp"
#include "backend/image_understanding_types.hpp"
#include "backend/library_server_types.hpp"
#include "backend/library_types.hpp"
#include "backend/people_analysis_types.hpp"
#include "backend/remote_library_types.hpp"
#include "backend/review_types.hpp"
#include "backend/semantic_search_types.hpp"
#include "backend/smart_category_types.hpp"
#include "folder_scan_backend.hpp"

#include <QString>
#include <QVariantList>
#include <QVector>

#include <cstdint>
#include <memory>
#include <optional>

class ExportBackend;

class DesktopBackend final {
  public:
    struct PipelineInput final {
        QString photo_id;
        QString representation_id;
        QString source_path;
        QString title;
    };

    DesktopBackend(const QString& catalog_path, const QString& cache_root);
    ~DesktopBackend();

    DesktopBackend(const DesktopBackend&) = delete;
    DesktopBackend& operator=(const DesktopBackend&) = delete;

    void beginFolderScan(std::uint64_t scan_id) const;
    [[nodiscard]] BackendScanReport
    scanFolder(const QString& folder_path, std::uint64_t scan_id) const;
    [[nodiscard]] BackendScanProgress scanProgress(std::uint64_t scan_id) const;
    [[nodiscard]] bool cancelFolderScan(std::uint64_t scan_id) const;
    [[nodiscard]] PipelineInput admitPipelineInput(const QString& source_path) const;
    [[nodiscard]] BackendPhotoInspection
    photoInspection(const QString& photo_id, const QString& representation_id) const;
    [[nodiscard]] BackendRemoteLibrarySnapshot
    remoteLibrarySnapshot(const QString& connection_id) const;
    [[nodiscard]] BackendRemoteLibrarySyncResult syncRemoteLibrary(
        const QString& connection_id,
        const QString& server_address,
        const QString& authorization
    ) const;
    void setRemoteLibraryReviewState(
        const QString& connection_id,
        const QString& remote_photo_id,
        const QString& remote_representation_id,
        BackendReviewDecisionFlag flag,
        std::uint8_t rating,
        bool liked,
        const QString& color_label,
        std::int64_t updated_at_ms
    ) const;
    [[nodiscard]] BackendRemoteLibraryMaterialization materializeRemoteLibraryPhoto(
        const QString& connection_id,
        const QString& server_address,
        const QString& authorization,
        const QString& remote_photo_id,
        const QString& remote_representation_id
    ) const;
    [[nodiscard]] BackendLibraryServerSnapshot libraryServerSnapshot() const;
    [[nodiscard]] BackendLibraryServerSnapshot
    startLibraryServer(const BackendLibraryServerConfig& config) const;
    [[nodiscard]] BackendLibraryServerSnapshot stopLibraryServer() const;
    [[nodiscard]] BackendLibraryServerSnapshot resetLibraryServerCache() const;
    [[nodiscard]] BackendLibraryPhotoPage libraryPhotoPage(
        const BackendLibraryPhotoFilter& filter,
        BackendLibraryPhotoOrder order,
        const BackendLibraryPhotoCursor& cursor,
        std::uint32_t limit
    ) const;
    [[nodiscard]] std::uint64_t libraryPhotoCount(const BackendLibraryPhotoFilter& filter) const;
    [[nodiscard]] QVector<BackendLocationReferenceAnchor> locationReferenceAnchors(
        std::int64_t capture_start_unix_seconds,
        std::int64_t capture_end_unix_seconds
    ) const;
    [[nodiscard]] QVector<BackendLocationReferenceLibrary> locationReferenceLibraries() const;
    [[nodiscard]] BackendLocationReferenceLibrary
    addLocationReferenceLibrary(const QString& root_path, std::int64_t clock_offset_seconds) const;
    [[nodiscard]] bool removeLocationReferenceLibrary(const QString& id) const;
    [[nodiscard]] BackendLibraryMapSnapshot libraryMapSnapshot(
        const BackendLibraryPhotoFilter& filter,
        const BackendLibraryMapViewport& viewport,
        const BackendLibraryMapGrid& grid
    ) const;
    [[nodiscard]] BackendLibraryFacetPage libraryFacetPage(
        const BackendLibraryPhotoFilter& filter,
        BackendLibraryFacetKind kind,
        const BackendLibraryFacetCursor& cursor,
        std::uint32_t limit
    ) const;
    [[nodiscard]] QVector<BackendLibraryPlaceResolutionCandidate>
    libraryPlaceResolutionCandidates(std::uint32_t limit) const;
    [[nodiscard]] BackendRecordLibraryPlaceResolutionStatus
    recordLibraryPlaceResolution(const BackendLibraryPlaceResolutionResult& result) const;
    [[nodiscard]] QVector<BackendLibraryAlbum> libraryAlbums() const;
    [[nodiscard]] QVector<BackendLibraryKeyword> libraryKeywords() const;
    [[nodiscard]] QVector<BackendLibraryPhotoKeyword>
    libraryKeywordsForPhoto(const QString& photo_id) const;
    [[nodiscard]] QVector<BackendLibrarySourceHealth> librarySourceHealth() const;
    [[nodiscard]] bool removeLibrarySource(const QString& source_id) const;
    [[nodiscard]] BackendMissingSourceLocationPage missingSourceLocationPage(
        const QString& scan_session_id,
        const QString& after_location_id,
        std::uint32_t limit
    ) const;
    [[nodiscard]] BackendVerifiedSourceRelinkReceipt relinkMissingSourceLocation(
        const QString& scan_session_id,
        const QString& location_id,
        const QString& candidate_path
    ) const;
    [[nodiscard]] BackendVerifiedSourceRelinkReceipt
    relinkLibrarySourceLocation(const QString& location_id, const QString& candidate_path) const;
    [[nodiscard]] BackendLibrarySourceRecoveryReceipt
    recoverLibrarySource(const QString& source_id, const QString& replacement_folder) const;
    [[nodiscard]] BackendSourceReconciliationReceipt
    reconcileMissingSourcePhotos(const QString& scan_session_id) const;
    [[nodiscard]] bool archiveLibraryPhoto(const QString& photo_id) const;
    [[nodiscard]] BackendLibraryAlbum createManualLibraryAlbum(const QString& name) const;
    [[nodiscard]] BackendLibraryKeyword
    createLibraryKeyword(const QString& parent_id, const QString& name) const;
    [[nodiscard]] BackendLibraryKeyword
    renameLibraryKeyword(const QString& keyword_id, const QString& name) const;
    [[nodiscard]] BackendLibraryKeyword
    moveLibraryKeyword(const QString& keyword_id, const QString& parent_id) const;
    [[nodiscard]] BackendLibraryKeywordDeletionReceipt
    deleteLibraryKeywordSubtree(const QString& keyword_id) const;
    [[nodiscard]] BackendLibraryKeywordMutationReceipt
    assignLibraryKeyword(const QString& keyword_id, const QStringList& photo_ids) const;
    [[nodiscard]] BackendLibraryKeywordMutationReceipt
    removeLibraryKeyword(const QString& keyword_id, const QStringList& photo_ids) const;
    [[nodiscard]] BackendLibraryAlbum createSmartLibraryAlbum(
        const QString& name,
        const BackendLibraryPhotoFilter& query_filter
    ) const;
    [[nodiscard]] BackendLibraryAlbum
    renameLibraryAlbum(const QString& album_id, const QString& name) const;
    [[nodiscard]] bool deleteLibraryAlbum(const QString& album_id) const;
    void addPhotoToManualLibraryAlbum(const QString& album_id, const QString& photo_id) const;
    [[nodiscard]] bool
    removePhotoFromManualLibraryAlbum(const QString& album_id, const QString& photo_id) const;
    [[nodiscard]] BackendPhotoLibraryState
    setPhotoLibraryState(const QString& photo_id, bool liked, const QString& color_label) const;
    [[nodiscard]] BackendLibraryMetadataState libraryMetadataState(const QString& photo_id) const;
    [[nodiscard]] BackendLibraryMetadataState setLibraryCaptureTimeOverride(
        const QString& photo_id,
        const QString& mode,
        std::int64_t captured_at_unix_seconds
    ) const;
    [[nodiscard]] BackendLibraryMetadataState setLibraryCoordinatesOverride(
        const QString& photo_id,
        const QString& mode,
        double latitude_degrees,
        double longitude_degrees,
        const QString& place_name
    ) const;
    [[nodiscard]] BackendCaptureTimeBatchPreview previewLibraryCaptureTimeBatch(
        const QVector<BackendBatchPhotoTarget>& targets,
        const QString& mode,
        std::int64_t offset_seconds
    ) const;
    [[nodiscard]] BackendLibraryMetadataBatchReceipt
    applyLibraryCaptureTimeBatch(const QString& preview_id) const;
    [[nodiscard]] BackendCoordinateBatchPreview previewLibraryCoordinateBatch(
        const QVector<BackendBatchPhotoTarget>& targets,
        const QString& mode,
        double latitude_degrees,
        double longitude_degrees,
        const QString& place_name,
        const QString& source_label
    ) const;
    [[nodiscard]] BackendLibraryMetadataBatchReceipt
    applyLibraryCoordinateBatch(const QString& preview_id) const;
    [[nodiscard]] BackendGpxImportPreview previewLibraryGpxImport(
        const QString& gpx_path,
        const QVector<BackendBatchPhotoTarget>& targets,
        std::int64_t camera_clock_offset_seconds,
        std::uint32_t maximum_gap_seconds
    ) const;
    [[nodiscard]] BackendLibraryMetadataBatchReceipt
    applyLibraryGpxImport(const QString& preview_id) const;
    [[nodiscard]] BackendReviewVisual loadReviewVisual(const QString& ticket) const;
    [[nodiscard]] std::uint32_t
    refreshSelectedReviewPreviews(const QStringList& visual_handles) const;
    [[nodiscard]] BackendReviewComparisonPresentation prepareReviewComparison(
        const QString& left_visual_handle,
        const QString& right_visual_handle
    ) const;
    void reportReviewVisualFrame(
        const QString& ticket,
        const QString& decoder_version,
        std::uint32_t requested_width,
        std::uint32_t requested_height,
        std::uint32_t decoded_width,
        std::uint32_t decoded_height,
        const QString& pixel_hash_hex
    ) const;
    void confirmReviewComparisonReady(
        const QString& presentation_id,
        const QString& left_request_ticket,
        const QString& right_request_ticket
    ) const;
    void cancelReviewComparison(const QString& presentation_id) const;
    [[nodiscard]] BackendFeedbackReceipt
    recordReviewComparison(const QString& presentation_id, BackendPairwiseOutcome outcome) const;
    [[nodiscard]] BackendForgetReceipt forgetReviewFeedback(const QString& event_id) const;
    [[nodiscard]] BackendReviewDecisionState
    reviewPhotoDecisionState(const QString& photo_id) const;
    [[nodiscard]] BackendReviewDecisionMutationReceipt setReviewPhotoDecision(
        const QString& photo_id,
        std::uint64_t expected_head_sequence,
        BackendReviewDecisionFlag desired_flag,
        std::uint8_t desired_rating
    ) const;
    [[nodiscard]] BackendPhotoEditState
    photoEditState(const QString& photo_id, const QString& source_path) const;
    [[nodiscard]] QByteArray exportShadowRecipe(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const BackendGradeStack& grade_stack,
        const QString& label
    ) const;
    [[nodiscard]] BackendShadowRecipeImportPreview
    previewShadowRecipe(const QByteArray& document) const;
    [[nodiscard]] BackendPhotoEditState
    resetIncompatiblePhotoEditHistory(const QString& photo_id, const QString& source_path) const;
    [[nodiscard]] QVariantList
    opticsProfileCandidates(const QString& photo_id, const QString& source_path) const;
    [[nodiscard]] QVector<BackendSharedGradeNode> sharedGradeNodes() const;
    [[nodiscard]] BackendSharedGradeNode
    publishSharedGradeNode(const QString& label, const BackendGradeNode& grade_node) const;
    [[nodiscard]] BackendBatchGradeReceipt applySharedGradeNodeToPhotos(
        const QString& layer_id,
        const QVector<BackendBatchPhotoTarget>& targets
    ) const;
    [[nodiscard]] BackendGradeNode newBasicGradeNode(const QString& label) const;
    /// Composition boundary for the durable export workflow. Controllers keep
    /// this component alive through the owning DesktopBackend session.
    [[nodiscard]] ExportBackend& exportBackend() noexcept;
    /// Reads cache state without deleting anything.
    [[nodiscard]] BackendCacheMaintenanceInventory cacheMaintenanceInventory() const;
    /// Calculates the conservative sweep candidates. Call this before asking
    /// the user to confirm a real maintenance action.
    [[nodiscard]] BackendCacheMaintenanceSweep planCacheMaintenanceSweep() const;
    /// Executes only the already user-confirmed conservative sweep. It never
    /// deletes unknown entries, live Catalog blobs, or recent writes.
    [[nodiscard]] BackendCacheMaintenanceSweep runCacheMaintenanceSweep() const;
    /// Loads the independently clearable device-local People Store. It is
    /// keyed to Catalog photo identities but contains no face embeddings.
    [[nodiscard]] BackendPeopleAnalysisReport peopleLibrarySnapshot() const;
    [[nodiscard]] std::uint64_t beginPeopleAnalysisJob(bool authorized) const;
    [[nodiscard]] BackendPeopleAnalysisProgress
    peopleAnalysisJobStatus(std::uint64_t job_token) const;
    [[nodiscard]] bool cancelPeopleAnalysisJob(std::uint64_t job_token) const;
    [[nodiscard]] BackendPeopleAnalysisExecution executePeopleAnalysisJob(
        std::uint64_t job_token,
        const QString& infer_base_url,
        const QString& credential_file,
        bool authorized
    ) const;
    void retirePeopleAnalysisJob(std::uint64_t job_token) const;
    [[nodiscard]] BackendPeopleAnalysisReport mergePeople(const QStringList& person_ids) const;
    [[nodiscard]] BackendPeopleAnalysisReport
    renamePerson(const QString& person_id, const QString& display_name) const;
    [[nodiscard]] BackendPeopleAnalysisReport undoPeopleMerge() const;
    void clearPeopleData() const;
    /// Executes bounded SigLIP text-to-image ranking over the current Review
    /// prefix. Results and exact visual tickets are session-only.
    [[nodiscard]] BackendSemanticSearchReport searchSemantics(
        const QString& infer_base_url,
        const QString& credential_file,
        const QString& query,
        const QString& query_revision,
        const QString& language
    ) const;
    [[nodiscard]] BackendSmartClassificationBatch classifySmartCategoriesBatch(
        const QString& infer_base_url,
        const QString& credential_file,
        const QVector<BackendSmartCategoryDefinition>& definitions,
        const QString& config_revision,
        const QString& generation,
        bool start_new,
        bool clear_embeddings
    ) const;
    [[nodiscard]] BackendSmartClassificationSnapshot smartClassificationSnapshot() const;
    [[nodiscard]] QStringList smartCategoryMembers(const QString& category_id) const;
    [[nodiscard]] QVector<BackendSmartCategoryReviewItem> smartCategoryReviewQueue() const;
    void setSmartCategoryFeedback(
        const QString& photo_id,
        const QString& representation_id,
        const QString& category_id,
        std::int8_t decision
    ) const;
    void completeSmartCategoryReview(
        const QString& photo_id,
        const QString& representation_id,
        const QVector<BackendSmartCategoryFeedbackDecision>& decisions
    ) const;
    void pauseSmartClassification(const QString& generation) const;
    [[nodiscard]] BackendImageUnderstandingBatch processImageUnderstandingBatch(
        const QString& infer_base_url,
        const QString& credential_file,
        const QString& scan_scope,
        std::uint8_t minimum_rating,
        const QString& generation,
        bool start_new,
        bool auto_apply_keywords
    ) const;
    [[nodiscard]] BackendImageUnderstandingSnapshot imageUnderstandingSnapshot() const;
    [[nodiscard]] BackendImageUnderstandingSnapshot
    pauseImageUnderstanding(const QString& generation) const;
    [[nodiscard]] BackendImageUnderstandingProposal
    imageUnderstandingProposal(const QString& photo_id, const QString& representation_id) const;
    void applyImageUnderstandingKeywords(
        const QString& photo_id,
        const QString& representation_id,
        const QString& source_revision
    ) const;
    [[nodiscard]] BackendClassificationReviewProposal reviewSmartClassificationWithModel(
        const QString& infer_base_url,
        const QString& credential_file,
        const QString& photo_id,
        const QString& representation_id,
        const QString& taxonomy_revision,
        const QVector<BackendClassificationReviewCategory>& categories
    ) const;
    [[nodiscard]] BackendClassificationReviewProposal
    advancedClassificationReview(const QString& photo_id, const QString& representation_id) const;
    [[nodiscard]] QString acceptAdvancedClassificationReview(
        const QString& photo_id,
        const QString& representation_id,
        const QString& source_revision
    ) const;
    void dismissAdvancedClassificationReview(
        const QString& photo_id,
        const QString& representation_id,
        const QString& source_revision
    ) const;
    [[nodiscard]] BackendEditedPreview renderEditPreview(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const BackendGradeStack& grade_stack,
        std::uint64_t render_token,
        std::uint32_t max_edge,
        std::uint8_t jpeg_quality,
        EditPreviewPolicy policy,
        std::optional<EditMaskCoverageRequest> mask_coverage_request
    ) const;
    [[nodiscard]] BackendRawWhiteBalancePickerResult pickRawWhiteBalance(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const BackendGradeStack& grade_stack,
        std::uint32_t max_edge,
        double normalized_x,
        double normalized_y
    ) const;
    [[nodiscard]] BackendRawWhiteBalancePickerResult autoRawWhiteBalance(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const BackendGradeStack& grade_stack,
        std::uint32_t max_edge
    ) const;
    [[nodiscard]] std::uint64_t beginEditPreviewRequest() const noexcept;
    [[nodiscard]] bool cancelEditPreviewRequest(std::uint64_t render_token) const noexcept;
    [[nodiscard]] std::uint64_t beginSubjectMaskInputSession() const;
    void finishSubjectMaskInputSession(std::uint64_t subject_mask_input_session_token) const;
    [[nodiscard]] std::uint64_t beginSubjectMaskJob() const;
    void cancelSubjectMaskJob(std::uint64_t subject_mask_job_token) const;
    [[nodiscard]] BackendSubjectMaskResult executeSubjectMaskJob(
        const QString& photo_id,
        const QString& source_path,
        const BackendSubjectMaskRequest& request
    ) const;
    [[nodiscard]] BackendPhotoEditState applySubjectMaskProposal(
        const QString& photo_id,
        const QString& source_path,
        const BackendSubjectMaskApplyRequest& request
    ) const;
    void discardSubjectMaskProposal(std::uint64_t proposal_token) const;
    [[nodiscard]] std::uint64_t beginImageCompletionJob() const;
    void cancelImageCompletionJob(std::uint64_t image_completion_job_token) const;
    [[nodiscard]] BackendImageCompletionResult executeImageCompletionJob(
        const QString& photo_id,
        const QString& source_path,
        const BackendImageCompletionRequest& request
    ) const;
    [[nodiscard]] BackendPhotoEditState applyImageCompletionProposal(
        const QString& photo_id,
        const QString& source_path,
        const BackendImageCompletionApplyRequest& request
    ) const;
    void discardImageCompletionProposal(std::uint64_t proposal_token) const;
    [[nodiscard]] BackendRawFoundationRuntimeStatus probeRawFoundationRuntime() const;
    [[nodiscard]] BackendRawFoundationNoiseAssessment
    assessRawFoundationNoise(const QString& photo_id, const QString& source_path) const;
    [[nodiscard]] std::uint64_t
    beginRawFoundationJob(const QString& request_id, std::uint64_t generation) const;
    void cancelRawFoundationJob(std::uint64_t raw_foundation_job_token) const;
    [[nodiscard]] BackendRawFoundationJobStatus
    rawFoundationJobStatus(std::uint64_t raw_foundation_job_token) const;
    [[nodiscard]] BackendRawFoundationJobStatus executeRawFoundationJob(
        std::uint64_t raw_foundation_job_token,
        const QString& photo_id,
        const QString& source_path
    ) const;
    void retireRawFoundationJob(std::uint64_t raw_foundation_job_token) const;
    [[nodiscard]] std::uint64_t beginEditDetailRequest() const noexcept;
    [[nodiscard]] BackendEditedDetailViewport renderEditDetailViewport(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const BackendGradeStack& grade_stack,
        std::uint64_t render_token,
        double center_x,
        double center_y,
        std::uint32_t viewport_width,
        std::uint32_t viewport_height,
        std::uint32_t tile_side,
        bool use_working_recipe
    ) const;
    [[nodiscard]] BackendPhotoEditState saveEditVersion(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const QString& expected_working_commit_id,
        const QString& expected_variant_id,
        const BackendGradeStack& grade_stack,
        const QString& version_name
    ) const;
    // Persists the current non-destructive working state without creating a
    // user-visible Library Version. The immutable Recipe commit advances only
    // the per-photo `working` ref, so autosave remains recoverable without
    // filling the Version panel with slider-level checkpoints.
    [[nodiscard]] BackendPhotoEditState autosaveWorkingEdit(
        const QString& photo_id,
        const QString& source_path,
        const QString& base_commit_id,
        const QString& expected_working_commit_id,
        const QString& expected_variant_id,
        const BackendGradeStack& grade_stack
    ) const;
    [[nodiscard]] BackendPhotoEditState createPhotoVariant(
        const QString& photo_id,
        const QString& source_path,
        const QString& name
    ) const;
    [[nodiscard]] BackendPhotoEditState renamePhotoVariant(
        const QString& photo_id,
        const QString& source_path,
        const QString& variant_id,
        const QString& name
    ) const;
    [[nodiscard]] BackendPhotoEditState activatePhotoVariant(
        const QString& photo_id,
        const QString& source_path,
        const QString& variant_id
    ) const;
    [[nodiscard]] BackendPhotoEditState removePhotoVariant(
        const QString& photo_id,
        const QString& source_path,
        const QString& variant_id
    ) const;
    [[nodiscard]] BackendPhotoEditState loadEditVersionDraft(
        const QString& photo_id,
        const QString& source_path,
        const QString& commit_id
    ) const;
    [[nodiscard]] BackendPhotoHistoryPage photoHistoryPage(
        const QString& photo_id,
        const BackendHistoryCursor& after,
        std::uint32_t limit
    ) const;
    [[nodiscard]] BackendLibraryHistoryPage
    libraryHistoryPage(const BackendHistoryCursor& after, std::uint32_t limit) const;
    [[nodiscard]] BackendLibraryHistoryRefPage
    libraryHistoryRefPage(const QString& after_name, std::uint32_t limit) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

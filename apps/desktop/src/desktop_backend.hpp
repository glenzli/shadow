#pragma once

#include "backend/cache_types.hpp"
#include "backend/edit_types.hpp"
#include "backend/library_types.hpp"
#include "backend/review_types.hpp"
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
    DesktopBackend(const QString& catalog_path, const QString& cache_root);
    ~DesktopBackend();

    DesktopBackend(const DesktopBackend&) = delete;
    DesktopBackend& operator=(const DesktopBackend&) = delete;

    void beginFolderScan(std::uint64_t scan_id) const;
    [[nodiscard]] BackendScanReport
    scanFolder(const QString& folder_path, std::uint64_t scan_id) const;
    [[nodiscard]] BackendScanProgress scanProgress(std::uint64_t scan_id) const;
    [[nodiscard]] bool cancelFolderScan(std::uint64_t scan_id) const;
    [[nodiscard]] BackendPhotoInspection
    photoInspection(const QString& photo_id, const QString& representation_id) const;
    [[nodiscard]] BackendLibraryPhotoPage libraryPhotoPage(
        const BackendLibraryPhotoFilter& filter,
        BackendLibraryPhotoOrder order,
        const BackendLibraryPhotoCursor& cursor,
        std::uint32_t limit
    ) const;
    [[nodiscard]] std::uint64_t libraryPhotoCount(const BackendLibraryPhotoFilter& filter) const;
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
    [[nodiscard]] BackendGpxImportPreview previewLibraryGpxImport(
        const QString& gpx_path,
        const QVector<BackendBatchPhotoTarget>& targets,
        std::int64_t camera_clock_offset_seconds,
        std::uint32_t maximum_gap_seconds
    ) const;
    [[nodiscard]] BackendLibraryMetadataBatchReceipt
    applyLibraryGpxImport(const QString& preview_id) const;
    [[nodiscard]] BackendReviewVisual loadReviewVisual(const QString& ticket) const;
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
    [[nodiscard]] std::uint64_t beginEditPreviewRequest() const noexcept;
    [[nodiscard]] bool cancelEditPreviewRequest(std::uint64_t render_token) const noexcept;
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
    [[nodiscard]] BackendRawFoundationRuntimeStatus probeRawFoundationRuntime() const;
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
        const BackendGradeStack& grade_stack
    ) const;
    [[nodiscard]] BackendPhotoEditState loadEditVersionDraft(
        const QString& photo_id,
        const QString& source_path,
        const QString& commit_id
    ) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

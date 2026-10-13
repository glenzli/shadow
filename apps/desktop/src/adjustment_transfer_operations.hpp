#pragma once

#include "backend/edit_types.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

class DesktopBackend;

enum class AdjustmentTransferIssue {
    None,
    Cancelled,
    NotFound,
    Unavailable,
    UnsupportedMask,
    Changed,
    NodeLimit,
    Failed
};

struct AdjustmentTransferFailure final {
    AdjustmentTransferIssue issue = AdjustmentTransferIssue::None;
    QString detail;
};

struct AdjustmentTransferSource final {
    QString photo_id, source_path, base_commit_id, name;
    BackendGradeStack stack;
    bool allow_spatial_masks = false;
};

struct AdjustmentTransferClipboard final {
    QByteArray document;
    QString source_photo_id, source_name;
    int node_count = 0;
    bool allow_spatial_masks = false;
    bool requires_inference = false;
};

// Cancellation owns the active inference handle, not a second image pipeline.
class AdjustmentTransferCancellation final {
  public:
    void cancel();
    void setActive(std::function<void()> callback);
    void clearActive();
    [[nodiscard]] bool cancelled() const noexcept {
        return cancelled_.load();
    }

  private:
    std::atomic_bool cancelled_{false};
    std::mutex mutex_;
    std::function<void()> cancel_active_;
};

struct AdjustmentTransferPrepared final {
    BackendPhotoEditState before;
    BackendGradeStack after;
    QString content_anchor;
    std::uint64_t plan_token = 0;
    // Keeps destination raster definitions in Rust until its atomic append
    // commit. The Qt mask marker alone cannot carry a new raster identity.
    std::shared_ptr<void> retained_plan;
    AdjustmentTransferFailure failure;
};

// Every operation runs on a worker. The controller reserves the editor only
// for the final metadata transaction, after checking the prepared snapshot.
struct AdjustmentTransferOperations final {
    std::function<AdjustmentTransferClipboard(const AdjustmentTransferSource&)> copy;
    std::function<BackendPhotoEditState(const BackendBatchPhotoTarget&)> load;
    std::function<AdjustmentTransferPrepared(
        const AdjustmentTransferClipboard&,
        const BackendBatchPhotoTarget&,
        const std::shared_ptr<AdjustmentTransferCancellation>&
    )>
        prepare;
    std::function<BackendPhotoEditState(const AdjustmentTransferPrepared&)> write;
    std::function<
        bool(const BackendPhotoEditState&, const std::shared_ptr<AdjustmentTransferCancellation>&)>
        preview;
};

[[nodiscard]] AdjustmentTransferOperations adjustmentTransferOperations(DesktopBackend& backend);

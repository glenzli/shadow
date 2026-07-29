#include "edit_preview_provider.hpp"

#include <QReadLocker>
#include <QWriteLocker>

#include <utility>

EditPreviewStore::StoredPreview& EditPreviewStore::slot(const EditPreviewSlot slot) noexcept {
    return slot == EditPreviewSlot::Before ? before_ : current_;
}

const EditPreviewStore::StoredPreview&
EditPreviewStore::slot(const EditPreviewSlot slot) const noexcept {
    return slot == EditPreviewSlot::Before ? before_ : current_;
}

void EditPreviewStore::publish(
    const EditPreviewSlot target,
    QByteArray bytes,
    const QSize dimensions,
    const qsizetype row_stride_bytes,
    QImage display_zebra,
    const quint64 generation,
    std::shared_ptr<const BackendEditPreviewFrame> frame,
    const EditPreviewPresentationBinding presentation_binding
) {
    QWriteLocker lock(&lock_);
    auto& stored = slot(target);
    stored.frame = std::move(frame);
    stored.bytes = std::move(bytes);
    stored.dimensions = dimensions;
    stored.row_stride_bytes = row_stride_bytes;
    stored.display_zebra = std::move(display_zebra);
    stored.generation = generation;
    stored.presentation_binding = presentation_binding;
}

void EditPreviewStore::clear(const EditPreviewSlot target, const quint64 generation) {
    QWriteLocker lock(&lock_);
    auto& stored = slot(target);
    stored = {};
    stored.generation = generation;
    if (target == EditPreviewSlot::Current) {
        mask_coverage_ = {};
        expected_mask_coverage_.reset();
    }
}

void EditPreviewStore::clearAll(const quint64 current_generation, const quint64 before_generation) {
    QWriteLocker lock(&lock_);
    current_ = {};
    current_.generation = current_generation;
    before_ = {};
    before_.generation = before_generation;
    mask_coverage_ = {};
    expected_mask_coverage_.reset();
}

EditPreviewStore::Snapshot
EditPreviewStore::snapshot(const EditPreviewSlot target, const quint64 generation) const {
    QReadLocker lock(&lock_);
    const auto& stored = slot(target);
    if (generation != stored.generation) {
        return {};
    }
    return {
        .frame = stored.frame,
        .bytes = stored.bytes,
        .dimensions = stored.dimensions,
        .row_stride_bytes = stored.row_stride_bytes,
        .display_zebra = stored.display_zebra,
        .presentation_binding = stored.presentation_binding,
    };
}

void EditPreviewStore::publishDetails(
    QVector<DetailPublication> publications,
    const EditDetailGeneration generation
) {
    QWriteLocker lock(&lock_);
    details_.clear();
    detail_generation_ = generation;
    for (auto& publication : publications) {
        if (publication.ticket.isEmpty() || publication.bytes.isEmpty()) {
            continue;
        }
        details_.insert(
            publication.ticket,
            StoredPreview{
                .bytes = std::move(publication.bytes),
                .dimensions = publication.dimensions,
                .generation = 0,
                .row_stride_bytes = publication.row_stride_bytes,
            }
        );
    }
}

void EditPreviewStore::clearDetails(const EditDetailGeneration generation) {
    QWriteLocker lock(&lock_);
    details_.clear();
    detail_generation_ = generation;
}

EditPreviewStore::Snapshot EditPreviewStore::detailSnapshot(
    const QString& ticket,
    const EditDetailGeneration generation
) const {
    QReadLocker lock(&lock_);
    if (generation != detail_generation_) {
        return {};
    }
    const auto found = details_.constFind(ticket);
    if (found == details_.cend()) {
        return {};
    }
    return {
        .frame = found->frame,
        .bytes = found->bytes,
        .dimensions = found->dimensions,
        .row_stride_bytes = found->row_stride_bytes,
        .display_zebra = found->display_zebra,
        .presentation_binding = found->presentation_binding,
    };
}

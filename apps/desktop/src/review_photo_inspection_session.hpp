#pragma once

#include <QString>

#include <QtTypes>

struct ReviewPhotoInspectionRequest final {
    QString photo_id;
    QString representation_id;
    quint64 generation = 0;

    [[nodiscard]] bool valid() const noexcept {
        return generation != 0
            && !photo_id.isEmpty()
            && !representation_id.isEmpty();
    }
};

/// Selection-owned generation guard for asynchronous photo inspection.
///
/// This state deliberately has no Library page generation. Pagination,
/// filtering, and delegate recycling cannot change the identity of an
/// explicit `{photo, representation}` request.
class ReviewPhotoInspectionSession final {
public:
    [[nodiscard]] ReviewPhotoInspectionRequest request(
        const QString& photo_id,
        const QString& representation_id
    ) {
        advanceGeneration();
        photo_id_ = photo_id;
        representation_id_ = representation_id;
        return current();
    }

    void clear() {
        advanceGeneration();
        photo_id_.clear();
        representation_id_.clear();
    }

    [[nodiscard]] ReviewPhotoInspectionRequest current() const {
        return {
            .photo_id = photo_id_,
            .representation_id = representation_id_,
            .generation = generation_,
        };
    }

    [[nodiscard]] bool accepts(
        const ReviewPhotoInspectionRequest& completed
    ) const noexcept {
        return completed.valid()
            && completed.generation == generation_
            && completed.photo_id == photo_id_
            && completed.representation_id == representation_id_;
    }

private:
    void advanceGeneration() noexcept {
        ++generation_;
        if (generation_ == 0) {
            ++generation_;
        }
    }

    QString photo_id_;
    QString representation_id_;
    quint64 generation_ = 0;
};

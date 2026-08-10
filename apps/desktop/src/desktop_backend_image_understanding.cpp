#include "backend/desktop_backend_private.hpp"
#include "backend/rust_qt_projection.hpp"

#include <utility>

namespace {

QStringList project_keywords(const rust::Vec<rust::String>& source) {
    QStringList keywords;
    keywords.reserve(
        desktop_backend_projection::checked_qt_vector_size(
            source.size(),
            "image-understanding keywords"
        )
    );
    for (const rust::String& keyword : source)
        keywords.push_back(desktop_backend_projection::qstring(keyword));
    return keywords;
}

BackendClassificationReviewProposal project_review(
    const shadow::desktop::FfiClassificationReviewProposal& source
) {
    return {
        .available = source.available,
        .photo_id = desktop_backend_projection::qstring(source.photo_id),
        .representation_id = desktop_backend_projection::qstring(source.representation_id),
        .source_revision = desktop_backend_projection::qstring(source.source_revision),
        .taxonomy_revision = desktop_backend_projection::qstring(source.taxonomy_revision),
        .disposition = desktop_backend_projection::qstring(source.disposition),
        .category_id = desktop_backend_projection::qstring(source.category_id),
        .proposal_status = desktop_backend_projection::qstring(source.proposal_status),
        .model_profile = desktop_backend_projection::qstring(source.model_profile),
        .model_build = desktop_backend_projection::qstring(source.model_build),
    };
}

} // namespace

BackendImageUnderstandingBatch DesktopBackend::processImageUnderstandingBatch(
    const QString& infer_base_url,
    const QString& credential_file,
    const QString& scan_scope,
    const std::uint8_t minimum_rating,
    const QString& generation,
    const bool start_new,
    const bool auto_apply_keywords
) const {
    const auto source = impl_->session->process_image_understanding_batch(
        infer_base_url.toStdString(),
        credential_file.toStdString(),
        scan_scope.toStdString(),
        minimum_rating,
        generation.toStdString(),
        start_new,
        auto_apply_keywords
    );
    return {
        .generation = desktop_backend_projection::qstring(source.generation),
        .status = desktop_backend_projection::qstring(source.status),
        .processed_photos = source.processed_photos,
        .total_photos = source.total_photos,
        .analyzed_photos = source.analyzed_photos,
        .reused_photos = source.reused_photos,
        .skipped_photos = source.skipped_photos,
    };
}

BackendImageUnderstandingSnapshot DesktopBackend::imageUnderstandingSnapshot() const {
    const auto source = impl_->session->image_understanding_snapshot();
    return {
        .available = source.available,
        .policy_revision = desktop_backend_projection::qstring(source.policy_revision),
        .generation = desktop_backend_projection::qstring(source.generation),
        .status = desktop_backend_projection::qstring(source.status),
        .processed_photos = source.processed_photos,
        .total_photos = source.total_photos,
        .updated_at_ms = source.updated_at_ms,
    };
}

BackendImageUnderstandingSnapshot DesktopBackend::pauseImageUnderstanding(
    const QString& generation
) const {
    const auto source = impl_->session->pause_image_understanding(generation.toStdString());
    return {
        .available = source.available,
        .policy_revision = desktop_backend_projection::qstring(source.policy_revision),
        .generation = desktop_backend_projection::qstring(source.generation),
        .status = desktop_backend_projection::qstring(source.status),
        .processed_photos = source.processed_photos,
        .total_photos = source.total_photos,
        .updated_at_ms = source.updated_at_ms,
    };
}

BackendImageUnderstandingProposal DesktopBackend::imageUnderstandingProposal(
    const QString& photo_id,
    const QString& representation_id
) const {
    const auto source = impl_->session->image_understanding_proposal(
        photo_id.toStdString(),
        representation_id.toStdString()
    );
    return {
        .available = source.available,
        .photo_id = desktop_backend_projection::qstring(source.photo_id),
        .representation_id = desktop_backend_projection::qstring(source.representation_id),
        .source_revision = desktop_backend_projection::qstring(source.source_revision),
        .description = desktop_backend_projection::qstring(source.description),
        .language = desktop_backend_projection::qstring(source.language),
        .keywords = project_keywords(source.keywords),
        .disposition = desktop_backend_projection::qstring(source.disposition),
        .model_profile = desktop_backend_projection::qstring(source.model_profile),
        .model_build = desktop_backend_projection::qstring(source.model_build),
    };
}

void DesktopBackend::applyImageUnderstandingKeywords(
    const QString& photo_id,
    const QString& representation_id,
    const QString& source_revision
) const {
    impl_->session->apply_image_understanding_keywords(
        photo_id.toStdString(),
        representation_id.toStdString(),
        source_revision.toStdString()
    );
}

BackendClassificationReviewProposal DesktopBackend::reviewSmartClassificationWithModel(
    const QString& infer_base_url,
    const QString& credential_file,
    const QString& photo_id,
    const QString& representation_id,
    const QString& taxonomy_revision,
    const QVector<BackendClassificationReviewCategory>& categories
) const {
    rust::Vec<shadow::desktop::FfiClassificationReviewCategory> ffi_categories;
    ffi_categories.reserve(static_cast<std::size_t>(categories.size()));
    for (const BackendClassificationReviewCategory& category : categories) {
        ffi_categories.push_back({
            .id = category.id.toStdString(),
            .name = category.name.toStdString(),
            .description = category.description.toStdString(),
        });
    }
    return project_review(impl_->session->review_smart_classification_with_model(
        infer_base_url.toStdString(),
        credential_file.toStdString(),
        photo_id.toStdString(),
        representation_id.toStdString(),
        taxonomy_revision.toStdString(),
        std::move(ffi_categories)
    ));
}

BackendClassificationReviewProposal DesktopBackend::advancedClassificationReview(
    const QString& photo_id,
    const QString& representation_id
) const {
    return project_review(impl_->session->advanced_classification_review(
        photo_id.toStdString(),
        representation_id.toStdString()
    ));
}

QString DesktopBackend::acceptAdvancedClassificationReview(
    const QString& photo_id,
    const QString& representation_id,
    const QString& source_revision
) const {
    return desktop_backend_projection::qstring(
        impl_->session->accept_advanced_classification_review(
            photo_id.toStdString(),
            representation_id.toStdString(),
            source_revision.toStdString()
        )
    );
}

void DesktopBackend::dismissAdvancedClassificationReview(
    const QString& photo_id,
    const QString& representation_id,
    const QString& source_revision
) const {
    impl_->session->dismiss_advanced_classification_review(
        photo_id.toStdString(),
        representation_id.toStdString(),
        source_revision.toStdString()
    );
}

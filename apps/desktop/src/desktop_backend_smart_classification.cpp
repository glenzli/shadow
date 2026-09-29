#include "backend/desktop_backend_private.hpp"
#include "backend/rust_qt_projection.hpp"

#include <utility>

BackendSmartClassificationBatch DesktopBackend::classifySmartCategoriesBatch(
    const QString& infer_base_url,
    const QString& credential_file,
    const QVector<BackendSmartCategoryDefinition>& definitions,
    const QString& config_revision,
    const QString& generation,
    const bool start_new,
    const bool clear_embeddings
) const {
    rust::Vec<shadow::desktop::FfiSmartCategoryDefinition> ffi_definitions;
    ffi_definitions.reserve(static_cast<std::size_t>(definitions.size()));
    for (const BackendSmartCategoryDefinition& definition : definitions) {
        ffi_definitions.push_back({
            .id = definition.id.toStdString(),
            .description = definition.description.toStdString(),
            .minimum_similarity = definition.minimum_similarity,
        });
    }
    const auto source = impl_->session->classify_smart_categories_batch(
        infer_base_url.toStdString(),
        credential_file.toStdString(),
        std::move(ffi_definitions),
        config_revision.toStdString(),
        generation.toStdString(),
        start_new,
        clear_embeddings
    );
    return {
        .config_revision = desktop_backend_projection::qstring(source.config_revision),
        .generation = desktop_backend_projection::qstring(source.generation),
        .status = desktop_backend_projection::qstring(source.status),
        .embedding_space = desktop_backend_projection::qstring(source.embedding_space),
        .model_build = desktop_backend_projection::qstring(source.model_build),
        .processed_photos = source.processed_photos,
        .embedded_photos = source.embedded_photos,
        .reused_photos = source.reused_photos,
        .skipped_photos = source.skipped_photos,
        .total_photos = source.total_photos,
    };
}

BackendSmartClassificationSnapshot DesktopBackend::smartClassificationSnapshot() const {
    const auto source = impl_->session->smart_classification_snapshot();
    QVector<BackendSmartCategoryCount> category_counts;
    category_counts.reserve(
        desktop_backend_projection::checked_qt_vector_size(
            source.category_counts.size(),
            "smart-category counts"
        )
    );
    for (const auto& count : source.category_counts) {
        category_counts.push_back({
            .category_id = desktop_backend_projection::qstring(count.category_id),
            .count = count.count,
        });
    }
    return {
        .config_revision = desktop_backend_projection::qstring(source.config_revision),
        .generation = desktop_backend_projection::qstring(source.generation),
        .status = desktop_backend_projection::qstring(source.status),
        .embedding_space = desktop_backend_projection::qstring(source.embedding_space),
        .model_build = desktop_backend_projection::qstring(source.model_build),
        .processed_photos = source.processed_photos,
        .total_photos = source.total_photos,
        .updated_at_ms = source.updated_at_ms,
        .has_published_results = source.has_published_results,
        .published_config_revision =
            desktop_backend_projection::qstring(source.published_config_revision),
        .published_at_ms = source.published_at_ms,
        .category_counts = std::move(category_counts),
        .uncertain_photos = source.uncertain_photos,
        .adaptation_pending = source.adaptation_pending,
    };
}

QStringList DesktopBackend::smartCategoryMembers(const QString& category_id) const {
    const rust::Vec<rust::String> source =
        impl_->session->smart_category_members(category_id.toStdString());
    QStringList members;
    members.reserve(
        desktop_backend_projection::checked_qt_vector_size(source.size(), "smart-category members")
    );
    for (const rust::String& member : source) {
        members.push_back(desktop_backend_projection::qstring(member));
    }
    return members;
}

QVector<BackendSmartCategoryReviewItem> DesktopBackend::smartCategoryReviewQueue() const {
    const rust::Vec<shadow::desktop::FfiSmartCategoryReviewItem> source =
        impl_->session->smart_category_review_queue();
    QVector<BackendSmartCategoryReviewItem> items;
    items.reserve(
        desktop_backend_projection::checked_qt_vector_size(
            source.size(),
            "smart-category review queue"
        )
    );
    for (const auto& item : source) {
        items.push_back({
            .photo_id = desktop_backend_projection::qstring(item.photo_id),
            .representation_id = desktop_backend_projection::qstring(item.representation_id),
            .category_id = desktop_backend_projection::qstring(item.category_id),
            .adapted_similarity = item.adapted_similarity,
            .decision_margin = item.decision_margin,
        });
    }
    return items;
}

void DesktopBackend::setSmartCategoryFeedback(
    const QString& photo_id,
    const QString& representation_id,
    const QString& category_id,
    const std::int8_t decision
) const {
    impl_->session->set_smart_category_feedback(
        photo_id.toStdString(),
        representation_id.toStdString(),
        category_id.toStdString(),
        decision
    );
}

void DesktopBackend::completeSmartCategoryReview(
    const QString& photo_id,
    const QString& representation_id,
    const QVector<BackendSmartCategoryFeedbackDecision>& decisions
) const {
    rust::Vec<rust::String> category_ids;
    rust::Vec<std::int8_t> ffi_decisions;
    category_ids.reserve(static_cast<std::size_t>(decisions.size()));
    ffi_decisions.reserve(static_cast<std::size_t>(decisions.size()));
    for (const BackendSmartCategoryFeedbackDecision& decision : decisions) {
        category_ids.push_back(decision.category_id.toStdString());
        ffi_decisions.push_back(decision.decision);
    }
    impl_->session->complete_smart_category_review(
        photo_id.toStdString(),
        representation_id.toStdString(),
        std::move(category_ids),
        std::move(ffi_decisions)
    );
}

void DesktopBackend::pauseSmartClassification(const QString& generation) const {
    impl_->session->pause_smart_classification(generation.toStdString());
}

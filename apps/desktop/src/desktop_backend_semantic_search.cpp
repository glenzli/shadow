#include "backend/desktop_backend_private.hpp"
#include "backend/rust_qt_projection.hpp"

#include <utility>

std::uint64_t DesktopBackend::beginSemanticSearch() const {
    return impl_->session->begin_semantic_search();
}
void DesktopBackend::cancelSemanticSearch(std::uint64_t token) const {
    impl_->session->cancel_semantic_search(token);
}

BackendSemanticSearchReport DesktopBackend::searchSemantics(
    const QString& infer_base_url,
    const QString& credential_file,
    const QString& query,
    const QString& query_revision,
    const QString& language,
    std::uint64_t token
) const {
    const auto source = impl_->session->search_semantics(
        infer_base_url.toStdString(),
        credential_file.toStdString(),
        query.toStdString(),
        query_revision.toStdString(),
        language.toStdString(),
        token
    );
    QVector<BackendSemanticSearchMatch> matches;
    matches.reserve(
        desktop_backend_projection::checked_qt_vector_size(
            source.matches.size(),
            "semantic search matches"
        )
    );
    for (const auto& semantic_match : source.matches) {
        matches.push_back({
            .photo_id = desktop_backend_projection::qstring(semantic_match.photo_id),
            .representation_id =
                desktop_backend_projection::qstring(semantic_match.representation_id),
            .cosine_similarity = semantic_match.cosine_similarity,
        });
    }
    return {
        .considered_photos = source.considered_photos,
        .embedded_photos = source.embedded_photos,
        .skipped_items = source.skipped_items,
        .truncated = source.truncated,
        .matches = std::move(matches),
    };
}

std::uint64_t DesktopBackend::beginSimilarReview() const {
    return impl_->session->begin_similar_review();
}

void DesktopBackend::cancelSimilarReview(std::uint64_t token) const {
    impl_->session->cancel_similar_review(token);
}

BackendSemanticSearchReport DesktopBackend::suggestSimilarReview(
    const QString& infer_base_url,
    const QString& credential_file,
    const QString& photo_id,
    const QString& representation_id,
    std::uint64_t token
) const {
    const auto source = impl_->session->suggest_similar_review(
        infer_base_url.toStdString(),
        credential_file.toStdString(),
        photo_id.toStdString(),
        representation_id.toStdString(),
        token
    );
    BackendSemanticSearchReport report;
    report.considered_photos = source.considered_photos;
    report.embedded_photos = source.embedded_photos;
    report.skipped_items = source.skipped_items;
    report.truncated = source.truncated;
    report.matches.reserve(
        desktop_backend_projection::checked_qt_vector_size(
            source.matches.size(),
            "similar review matches"
        )
    );
    for (const auto& match : source.matches) {
        report.matches.push_back({
            .photo_id = desktop_backend_projection::qstring(match.photo_id),
            .representation_id = desktop_backend_projection::qstring(match.representation_id),
            .cosine_similarity = match.cosine_similarity,
        });
    }
    return report;
}

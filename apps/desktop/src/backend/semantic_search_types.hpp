#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

struct BackendSemanticSearchMatch final {
    QString photo_id;
    QString representation_id;
    float cosine_similarity = 0.0F;
};

/// Session-only semantic ranking. Vectors, model prompts, and local paths stay
/// behind the Rust bridge; Qt receives only exact visual tickets and scores.
struct BackendSemanticSearchReport final {
    std::uint32_t considered_photos = 0;
    std::uint32_t embedded_photos = 0;
    std::uint32_t skipped_items = 0;
    bool truncated = false;
    QVector<BackendSemanticSearchMatch> matches;
};

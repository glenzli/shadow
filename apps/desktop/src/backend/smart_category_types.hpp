#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

struct BackendSmartCategoryDefinition final {
    QString id;
    QString description;
    float minimum_similarity = 0.20F;
};

struct BackendSmartCategoryCount final {
    QString category_id;
    std::uint64_t count = 0;
};

struct BackendSmartClassificationSnapshot final {
    QString config_revision;
    QString generation;
    QString status;
    QString embedding_space;
    QString model_build;
    std::uint64_t processed_photos = 0;
    std::uint64_t total_photos = 0;
    std::int64_t updated_at_ms = 0;
    bool has_published_results = false;
    QString published_config_revision;
    std::int64_t published_at_ms = 0;
    QVector<BackendSmartCategoryCount> category_counts;
    std::uint64_t uncertain_photos = 0;
    bool adaptation_pending = false;
};

struct BackendSmartCategoryReviewItem final {
    QString photo_id;
    QString representation_id;
    QString category_id;
    float adapted_similarity = 0.0F;
    float decision_margin = 0.0F;
};

struct BackendSmartCategoryFeedbackDecision final {
    QString category_id;
    std::int8_t decision = 0;
};

struct BackendSmartClassificationBatch final {
    QString config_revision;
    QString generation;
    QString status;
    QString embedding_space;
    QString model_build;
    std::uint64_t processed_photos = 0;
    std::uint32_t embedded_photos = 0;
    std::uint32_t reused_photos = 0;
    std::uint32_t skipped_photos = 0;
    std::uint64_t total_photos = 0;
};

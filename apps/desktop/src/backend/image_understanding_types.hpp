#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

struct BackendImageUnderstandingBatch final {
    QString generation;
    QString status;
    std::uint64_t processed_photos = 0;
    std::uint64_t total_photos = 0;
    std::uint32_t analyzed_photos = 0;
    std::uint32_t reused_photos = 0;
    std::uint32_t skipped_photos = 0;
};

struct BackendImageUnderstandingSnapshot final {
    bool available = false;
    QString policy_revision;
    QString generation;
    QString status;
    std::uint64_t processed_photos = 0;
    std::uint64_t total_photos = 0;
    std::int64_t updated_at_ms = 0;
};

struct BackendImageUnderstandingProposal final {
    bool available = false;
    QString photo_id;
    QString representation_id;
    QString source_revision;
    QString description;
    QString language;
    QStringList keywords;
    QString disposition;
    QString model_profile;
    QString model_build;
};

struct BackendClassificationReviewCategory final {
    QString id;
    QString name;
    QString description;
};

struct BackendClassificationReviewProposal final {
    bool available = false;
    QString photo_id;
    QString representation_id;
    QString source_revision;
    QString taxonomy_revision;
    QString disposition;
    QString category_id;
    QString proposal_status;
    QString model_profile;
    QString model_build;
};

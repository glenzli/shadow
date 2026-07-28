#pragma once

#include <QString>
#include <QtGlobal>

#include <optional>

/// Lifetime declared by a Review image-provider URL.
///
/// A grid ticket authenticates immutable bytes and may finish while the Library
/// advances to another presentation generation. A comparison ticket also owns
/// a decoded-frame receipt, so it must fail closed as soon as its generation is
/// no longer current.
enum class ReviewVisualLifetime {
    Grid,
    Comparison,
};

struct ReviewVisualRequest final {
    ReviewVisualLifetime lifetime = ReviewVisualLifetime::Grid;
    quint64 generation = 0;
    QString ticket;

    [[nodiscard]] bool requiresCurrentGeneration() const noexcept {
        return lifetime == ReviewVisualLifetime::Comparison;
    }

    [[nodiscard]] bool requiresFrameReceipt() const noexcept {
        return lifetime == ReviewVisualLifetime::Comparison;
    }
};

/// Produces the one canonical URL grammar consumed by ThumbnailProvider.
[[nodiscard]] QString reviewVisualSource(
    const QString& ticket,
    quint64 generation,
    ReviewVisualLifetime lifetime
);

/// Parses the provider id (`visual?...`) and rejects ambiguous or extra fields.
[[nodiscard]] std::optional<ReviewVisualRequest> parseReviewVisualRequest(
    const QString& id
);

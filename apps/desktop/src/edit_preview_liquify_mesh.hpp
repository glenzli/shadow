#pragma once

#include <QPointF>
#include <QRectF>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

struct EditPreviewLiquifyVertex final {
    float x = 0.0F;
    float y = 0.0F;
    float texture_x = 0.0F;
    float texture_y = 0.0F;

    auto operator<=>(const EditPreviewLiquifyVertex&) const = default;
};

struct EditPreviewLiquifySample final {
    QPointF point;
    double pressure = 1.0;
};

/// Mutable display-only mesh for one active Liquify push gesture.
///
/// Positions deform incrementally while texture coordinates retain the
/// authoritative settled preview. The outer ring stays pinned so the warped
/// mesh continues to cover the complete preview rectangle. This object never
/// owns Recipe state and is discarded when the committed preview arrives.
class EditPreviewLiquifyMesh final {
  public:
    static constexpr std::uint16_t GRID_COLUMNS = 128U;
    static constexpr std::uint16_t GRID_ROWS = 128U;
    static constexpr std::size_t MAXIMUM_PREVIEW_STAMPS_PER_SEGMENT = 128U;

    void reset(QRectF target_rect, QRectF texture_rect);

    [[nodiscard]] bool appendNormalizedPoint(
        QPointF point,
        double pressure,
        double radius,
        double strength,
        double hardness
    );

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool deformed() const noexcept;
    [[nodiscard]] std::size_t pointCount() const noexcept;
    [[nodiscard]] QRectF targetRect() const noexcept;
    [[nodiscard]] QRectF textureRect() const noexcept;
    [[nodiscard]] std::span<const EditPreviewLiquifyVertex> vertices() const noexcept;
    [[nodiscard]] std::span<const std::uint16_t> indices() const noexcept;

  private:
    void applyPushSegment(
        EditPreviewLiquifySample from,
        EditPreviewLiquifySample to,
        double radius,
        double strength,
        double hardness
    );
    void applyStamp(QPointF center, QPointF displacement, double radius, double hardness);

    QRectF target_rect_;
    QRectF texture_rect_;
    std::vector<EditPreviewLiquifyVertex> vertices_;
    std::vector<std::uint16_t> indices_;
    std::optional<EditPreviewLiquifySample> last_sample_;
    std::size_t point_count_ = 0U;
    bool deformed_ = false;
};

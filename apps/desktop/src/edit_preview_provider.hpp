#pragma once

#include "backend/edit_preview_frame.hpp"
#include "edit_mask_coverage_contract.hpp"
#include "edit_preview_contract.hpp"

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QQuickImageProvider>
#include <QReadWriteLock>
#include <QSize>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>
#include <optional>

class EditPreviewPresentationContext;

enum class EditPreviewSlot : std::uint8_t {
    Current,
    Before,
};

class EditPreviewStore final {
public:
    struct Snapshot final {
        std::shared_ptr<const BackendEditPreviewFrame> frame;
        QByteArray bytes;
        QSize dimensions;
        qsizetype row_stride_bytes = 0;
        QImage display_zebra;
        EditPreviewPresentationBinding presentation_binding;
    };

    struct DetailPublication final {
        QString ticket;
        QByteArray bytes;
        QSize dimensions;
        qsizetype row_stride_bytes = 0;
    };

    struct MaskCoverageSnapshot final {
        std::shared_ptr<const BackendEditPreviewFrame> frame;
        QByteArray samples;
        QSize dimensions;
        qsizetype row_stride_bytes = 0;
    };

    void publish(
        EditPreviewSlot slot,
        QByteArray bytes,
        QSize dimensions,
        qsizetype row_stride_bytes,
        QImage display_zebra,
        quint64 generation,
        std::shared_ptr<const BackendEditPreviewFrame> frame = {},
        EditPreviewPresentationBinding presentation_binding = {}
    );
    void clear(EditPreviewSlot slot, quint64 generation);
    void clearAll(quint64 current_generation, quint64 before_generation);
    [[nodiscard]] Snapshot snapshot(EditPreviewSlot slot, quint64 generation) const;
    void publishDetails(
        QVector<DetailPublication> publications,
        EditDetailGeneration generation
    );
    void clearDetails(EditDetailGeneration generation);
    [[nodiscard]] Snapshot detailSnapshot(
        const QString& ticket,
        EditDetailGeneration generation
    ) const;
    void expectMaskCoverage(MaskCoverageGeneration generation);
    void clearMaskCoverage();
    [[nodiscard]] bool publishMaskCoverage(
        EditMaskCoveragePayload payload,
        MaskCoverageGeneration generation
    );
    [[nodiscard]] MaskCoverageSnapshot maskCoverageSnapshot(
        MaskCoverageGeneration generation
    ) const;

private:
    struct StoredPreview final {
        std::shared_ptr<const BackendEditPreviewFrame> frame;
        QByteArray bytes;
        QSize dimensions;
        quint64 generation = 0;
        qsizetype row_stride_bytes = 0;
        QImage display_zebra;
        EditPreviewPresentationBinding presentation_binding;
    };

    struct StoredMaskCoverage final {
        std::shared_ptr<const BackendEditPreviewFrame> frame;
        QByteArray samples;
        QSize dimensions;
        qsizetype row_stride_bytes = 0;
        MaskCoverageGeneration generation;
        bool available = false;
    };

    [[nodiscard]] StoredPreview& slot(EditPreviewSlot slot) noexcept;
    [[nodiscard]] const StoredPreview& slot(EditPreviewSlot slot) const noexcept;

    mutable QReadWriteLock lock_;
    StoredPreview current_;
    StoredPreview before_;
    QHash<QString, StoredPreview> details_;
    EditDetailGeneration detail_generation_;
    StoredMaskCoverage mask_coverage_;
    std::optional<MaskCoverageGeneration> expected_mask_coverage_;
};

class EditPreviewProvider final : public QQuickImageProvider {
public:
  explicit EditPreviewProvider(
      std::shared_ptr<EditPreviewStore> store,
      std::shared_ptr<EditPreviewPresentationContext> presentation_context = {}
  );

  [[nodiscard]] QImage
  requestImage(const QString& id, QSize* size, const QSize& requested_size) override;
  [[nodiscard]] QQuickTextureFactory*
  requestTexture(const QString& id, QSize* size, const QSize& requested_size) override;

private:
  [[nodiscard]] QImage resolveImage(
      const QString& id,
      QSize* size,
      const QSize& requested_size,
      std::shared_ptr<const BackendEditPreviewFrame>* retained_frame
  ) const;

  std::shared_ptr<EditPreviewStore> store_;
  std::shared_ptr<EditPreviewPresentationContext> presentation_context_;
};

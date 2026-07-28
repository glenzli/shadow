#include "backend/export_backend.hpp"

#include "shadow-desktop-bridge/src/lib.rs.h"

#include <QColorSpace>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

[[nodiscard]] QString qstring(const rust::String& value) {
    const auto length = std::min<std::size_t>(
        value.size(),
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())
    );
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(length));
}

[[nodiscard]] qsizetype checked_qt_vector_size(
    const std::size_t size,
    const char* const field
) {
    if (size > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
        throw std::length_error(
            std::string("desktop bridge vector is too large: ") + field
        );
    }
    return static_cast<qsizetype>(size);
}

class ExportOutputConflict final : public std::runtime_error {
public:
    explicit ExportOutputConflict(const QString& destination_path)
        : std::runtime_error(
              std::string("export destination already exists: ")
              + destination_path.toStdString()
          ) {}
};

[[nodiscard]] shadow::desktop::FfiDurableExportItemState ffi_durable_export_stage(
    const std::uint8_t stage
) {
    switch (stage) {
    case 0:
        return shadow::desktop::FfiDurableExportItemState::Preparing;
    case 1:
        return shadow::desktop::FfiDurableExportItemState::Rendering;
    case 2:
        return shadow::desktop::FfiDurableExportItemState::Encoding;
    case 3:
        return shadow::desktop::FfiDurableExportItemState::WritingTemp;
    }
    throw std::invalid_argument("unknown durable export stage");
}

[[nodiscard]] QString export_receipt_json(
    const BackendExportOptions& options,
    const QImage& image
) {
    return QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("schema"), 1},
        {QStringLiteral("format"), options.format},
        {QStringLiteral("width"), image.width()},
        {QStringLiteral("height"), image.height()},
        {QStringLiteral("max_edge"), static_cast<qint64>(options.max_edge)},
        {QStringLiteral("jpeg_quality"), static_cast<int>(options.jpeg_quality)},
        {QStringLiteral("watermark_applied"), !options.watermark_path.isEmpty()},
    }).toJson(QJsonDocument::Compact));
}

[[nodiscard]] BackendExportReceipt encode_export_raster(
    const shadow::desktop::FfiEditedExportRaster& raster,
    const QString& destination_path,
    const BackendExportOptions& options
) {
    if (destination_path.isEmpty()) {
        throw std::invalid_argument("export destination path is empty");
    }
    ExportSettingsCodec::validate(options);
    const std::uint64_t expected_row_stride =
        static_cast<std::uint64_t>(raster.width) * 3U;
    const std::uint64_t required_byte_count =
        static_cast<std::uint64_t>(raster.row_stride_bytes) * raster.height;
    if (raster.width == 0 || raster.height == 0
        || expected_row_stride > std::numeric_limits<std::uint32_t>::max()
        || raster.row_stride_bytes != expected_row_stride
        || raster.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || raster.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || required_byte_count > static_cast<std::uint64_t>(raster.bytes.size())) {
        throw std::runtime_error("export renderer returned an invalid RGB8 raster");
    }
    const qsizetype byte_count =
        checked_qt_vector_size(raster.bytes.size(), "export_raster");
    QImage image(
        raster.bytes.data(),
        static_cast<int>(raster.width),
        static_cast<int>(raster.height),
        static_cast<qsizetype>(raster.row_stride_bytes),
        QImage::Format_RGB888
    );
    image = image.copy();
    if (image.isNull() || image.sizeInBytes() > byte_count) {
        throw std::runtime_error("could not materialize the rendered export raster");
    }
    image.setColorSpace(QColorSpace::SRgb);
    if (options.max_edge > 0
        && static_cast<std::uint32_t>(
            std::max(image.width(), image.height())
        ) > options.max_edge) {
        image = image.scaled(
            QSize(
                static_cast<int>(options.max_edge),
                static_cast<int>(options.max_edge)
            ),
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation
        );
    }
    if (!options.watermark_path.isEmpty()) {
        QImageReader watermark_reader(options.watermark_path);
        watermark_reader.setAutoTransform(true);
        QImage watermark = watermark_reader.read();
        if (watermark.isNull()) {
            throw std::runtime_error(
                std::string("could not read PNG watermark: ")
                + watermark_reader.errorString().toStdString()
            );
        }
        const int watermark_width = std::clamp(
            qRound(static_cast<double>(image.width())
                   * std::clamp(options.watermark_scale, 0.01, 1.0)),
            1,
            image.width()
        );
        watermark = watermark.scaledToWidth(
            watermark_width,
            Qt::SmoothTransformation
        );
        const int inset = qRound(
            static_cast<double>(std::min(image.width(), image.height()))
            * std::clamp(options.watermark_inset, 0.0, 0.25)
        );
        const bool left = options.watermark_anchor.endsWith(QStringLiteral("left"));
        const bool right = options.watermark_anchor.endsWith(QStringLiteral("right"));
        const bool top = options.watermark_anchor.startsWith(QStringLiteral("top"));
        const bool bottom =
            options.watermark_anchor.startsWith(QStringLiteral("bottom"));
        const int x = left ? inset
            : right ? image.width() - watermark.width() - inset
                    : (image.width() - watermark.width()) / 2;
        const int y = top ? inset
            : bottom ? image.height() - watermark.height() - inset
                     : (image.height() - watermark.height()) / 2;
        QPainter painter(&image);
        painter.setOpacity(std::clamp(options.watermark_opacity, 0.0, 1.0));
        painter.drawImage(QPoint(std::max(0, x), std::max(0, y)), watermark);
        painter.end();
    }
    if (QFileInfo::exists(destination_path)) {
        throw ExportOutputConflict(destination_path);
    }
    QSaveFile destination(destination_path);
    // A durable queue records completion only after atomic publication. Never
    // silently fall back to an in-place write if the filesystem cannot stage
    // and rename the temporary output alongside its destination.
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly)) {
        throw std::runtime_error(
            std::string("could not open export destination: ")
            + destination.errorString().toStdString()
        );
    }
    QImageWriter writer(
        &destination,
        options.format == QStringLiteral("png")
            ? QByteArrayLiteral("png") : QByteArrayLiteral("jpg")
    );
    if (options.format == QStringLiteral("jpeg")) {
        writer.setQuality(options.jpeg_quality);
        writer.setOptimizedWrite(true);
    }
    if (!writer.write(image)) {
        destination.cancelWriting();
        throw std::runtime_error(
            std::string("could not encode export: ")
            + writer.errorString().toStdString()
        );
    }
    const std::uint64_t byte_length =
        static_cast<std::uint64_t>(destination.size());
    if (!destination.commit()) {
        throw std::runtime_error(
            std::string("could not publish export atomically: ")
            + destination.errorString().toStdString()
        );
    }
    return {
        .destination_path = destination_path,
        .width = static_cast<std::uint32_t>(image.width()),
        .height = static_cast<std::uint32_t>(image.height()),
        .byte_length = byte_length,
        .output_format = options.format,
        .receipt_json = export_receipt_json(options, image),
    };
}

} // namespace

ExportBackend::ExportBackend(
    shadow::desktop::DesktopSession& session
) noexcept
    : session_(&session) {}

BackendDurableExportJob ExportBackend::enqueueDurableExportJob(
    const QVector<BackendDurableExportTarget>& targets,
    const QString& settings_json
) const {
    rust::Vec<shadow::desktop::FfiDurableExportTarget> ffi_targets;
    ffi_targets.reserve(static_cast<std::size_t>(targets.size()));
    for (const auto& target : targets) {
        shadow::desktop::FfiDurableExportTarget ffi_target;
        ffi_target.photo_id = target.photo_id.toStdString();
        ffi_target.source_path = target.source_path.toStdString();
        ffi_target.output_path = target.output_path.toStdString();
        ffi_targets.push_back(std::move(ffi_target));
    }
    const auto job = session_->enqueue_durable_export_job(
        std::move(ffi_targets),
        settings_json.toStdString()
    );
    return {
        .job_id = qstring(job.job_id),
        .item_count = job.item_count,
    };
}

BackendDurableExportRecovery ExportBackend::recoverDurableExportQueue() const {
    const auto recovery = session_->recover_durable_export_queue();
    return {
        .interrupted_items = recovery.interrupted_items,
        .requeued_items = recovery.requeued_items,
        .queued_items = recovery.queued_items,
    };
}

std::optional<BackendDurableExportItem>
ExportBackend::claimNextDurableExportItem() const {
    const auto item = session_->claim_next_durable_export_item();
    if (!item.has_item) {
        return std::nullopt;
    }
    return BackendDurableExportItem{
        .item_id = qstring(item.item_id),
        .job_id = qstring(item.job_id),
        .photo_id = qstring(item.photo_id),
        .source_path = qstring(item.source_path),
        .output_path = qstring(item.output_path),
        .settings_json = qstring(item.settings_json),
    };
}

BackendExportReceipt ExportBackend::executeDurableExportItem(
    const BackendDurableExportItem& item
) const {
    std::uint8_t stage = 0;
    try {
        // Decode the persisted settings before spending time on RAW render.
        // A malformed snapshot is a terminal item error, not a reason to
        // repeatedly render an image on every retry.
        const BackendExportOptions options =
            ExportSettingsCodec::fromDurableJson(item.settings_json);
        session_->begin_durable_export_render(item.item_id.toStdString());
        stage = 1;
        shadow::desktop::FfiDurableExportItem ffi_item;
        ffi_item.has_item = true;
        ffi_item.item_id = item.item_id.toStdString();
        ffi_item.job_id = item.job_id.toStdString();
        ffi_item.photo_id = item.photo_id.toStdString();
        ffi_item.source_path = item.source_path.toStdString();
        ffi_item.output_path = item.output_path.toStdString();
        ffi_item.settings_json = item.settings_json.toStdString();
        const auto raster = session_->render_durable_export_item(ffi_item);

        session_->begin_durable_export_encoding(item.item_id.toStdString());
        stage = 2;

        session_->begin_durable_export_write(item.item_id.toStdString());
        stage = 3;
        const BackendExportReceipt receipt = encode_export_raster(
            raster,
            item.output_path,
            options
        );
        session_->complete_durable_export_item(
            item.item_id.toStdString(),
            item.job_id.toStdString(),
            receipt.output_format.toStdString(),
            receipt.byte_length,
            receipt.receipt_json.toStdString()
        );
        return receipt;
    } catch (const ExportOutputConflict&) {
        // We do not overwrite a path that appeared after the immutable job
        // was created. Preserve the queue item so a future conflict UI can
        // offer rename, replace, or skip without rerendering by accident.
        try {
            session_->pause_durable_export_conflict(item.item_id.toStdString());
        } catch (...) {
            // Leave the item in WritingTemp if the catalog itself is
            // temporarily unavailable; startup recovery will make it safe to
            // retry rather than losing the write conflict.
        }
        throw;
    } catch (const std::exception& error) {
        try {
            session_->fail_durable_export_item(
                item.item_id.toStdString(),
                ffi_durable_export_stage(stage),
                "desktop_export_failed",
                error.what(),
                true
            );
        } catch (...) {
            // The original renderer/encoder error remains more useful to the
            // caller. Recovery will convert any in-flight state to queued on
            // the next startup if recording the failure also failed.
        }
        throw;
    }
}

void ExportBackend::cancelDurableExportJob(const QString& job_id) const {
    session_->cancel_durable_export_job(job_id.toStdString());
}

BackendDurableExportProgress ExportBackend::durableExportProgress(
    const QString& job_id
) const {
    const auto progress = session_->durable_export_progress(job_id.toStdString());
    return {
        .queued = progress.queued,
        .active = progress.active,
        .completed = progress.completed,
        .failed = progress.failed,
        .cancelled = progress.cancelled,
        .paused_conflict = progress.paused_conflict,
        .total = progress.total,
    };
}

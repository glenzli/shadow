#pragma once

#include "backend/library_types.hpp"
#include "backend/edit_types.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <cstdint>
#include <functional>

struct ReviewLibraryMetadataTaskResult final {
    enum class Kind : std::uint8_t {
        Load,
        CaptureMutation,
        CoordinatesMutation,
        CaptureBatchPreview,
        CaptureBatchApply,
        GpxPreview,
        GpxApply,
    };

    Kind kind = Kind::Load;
    BackendLibraryMetadataState metadata;
    BackendCaptureTimeBatchPreview capture_time_preview;
    BackendGpxImportPreview gpx_preview;
    BackendLibraryMetadataBatchReceipt batch_receipt;
    QString error;
};

/// Owns asynchronous metadata correction and GPX preview/apply work.
///
/// ReviewController is only the QML facade. Parsing GPX and writing a large
/// correction batch must never occupy the GUI thread.
class ReviewLibraryMetadataCoordinator final : public QObject {
    Q_OBJECT

public:
    struct Operations final {
        std::function<BackendLibraryMetadataState(const QString&)> load;
        std::function<BackendLibraryMetadataState(
            const QString&,
            const QString&,
            std::int64_t
        )> set_capture_time;
        std::function<BackendLibraryMetadataState(
            const QString&,
            const QString&,
            double,
            double,
            const QString&
        )> set_coordinates;
        std::function<BackendCaptureTimeBatchPreview(
            const QVector<BackendBatchPhotoTarget>&,
            const QString&,
            std::int64_t
        )> preview_capture_time;
        std::function<BackendLibraryMetadataBatchReceipt(const QString&)>
            apply_capture_time;
        std::function<BackendGpxImportPreview(
            const QString&,
            const QVector<BackendBatchPhotoTarget>&,
            std::int64_t,
            std::uint32_t
        )> preview_gpx;
        std::function<BackendLibraryMetadataBatchReceipt(const QString&)>
            apply_gpx;
    };

    explicit ReviewLibraryMetadataCoordinator(
        Operations operations,
        QObject* parent = nullptr
    );
    ~ReviewLibraryMetadataCoordinator() override;

    [[nodiscard]] QVariantMap metadata() const;
    [[nodiscard]] QVariantMap captureTimePreview() const;
    [[nodiscard]] QVariantMap gpxPreview() const;
    [[nodiscard]] QVariantMap batchReceipt() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString statusCode() const;
    [[nodiscard]] QString errorText() const;

    void request(const QString& photo_id);
    void clear();
    void setCaptureTime(
        const QString& photo_id,
        const QString& mode,
        std::int64_t captured_at_unix_seconds
    );
    void setCoordinates(
        const QString& photo_id,
        const QString& mode,
        double latitude_degrees,
        double longitude_degrees,
        const QString& place_name
    );
    void previewCaptureTime(
        const QVariantList& targets,
        const QString& mode,
        std::int64_t offset_seconds
    );
    void applyCaptureTime(const QString& preview_id);
    void previewGpx(
        const QString& gpx_path,
        const QVariantList& targets,
        std::int64_t camera_clock_offset_seconds,
        std::uint32_t maximum_gap_seconds
    );
    void applyGpx(const QString& preview_id);

signals:
    void stateChanged();
    void libraryChanged(const QString& photoId);

private:
    void start(
        ReviewLibraryMetadataTaskResult::Kind kind,
        QString status_code,
        std::function<ReviewLibraryMetadataTaskResult()> task
    );
    void finish();
    [[nodiscard]] static QVector<BackendBatchPhotoTarget>
    batchTargets(const QVariantList& targets);

    Operations operations_;
    BackendLibraryMetadataState metadata_;
    BackendCaptureTimeBatchPreview capture_time_preview_;
    BackendGpxImportPreview gpx_preview_;
    BackendLibraryMetadataBatchReceipt batch_receipt_;
    QString status_code_ = QStringLiteral("idle");
    QString error_;
    bool running_ = false;
    QFutureWatcher<ReviewLibraryMetadataTaskResult> watcher_;
};

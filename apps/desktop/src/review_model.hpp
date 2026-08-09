#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <optional>

struct ReviewDecisionValue final {
    quint64 head_sequence = 0;
    QString flag = QStringLiteral("unflagged");
    int rating = 0;

    bool operator==(const ReviewDecisionValue&) const = default;
};

struct ReviewLibraryStateValue final {
    bool liked = false;
    QString color_label = QStringLiteral("none");
    std::int64_t updated_at_ms = 0;

    bool operator==(const ReviewLibraryStateValue&) const = default;
};

/// Immutable input to the asynchronous local-source availability monitor.
/// The location and path are retained so a late filesystem result cannot be
/// applied after the same logical photo has been relinked elsewhere.
struct ReviewLocalSourceProbe final {
    QString photo_id;
    QString location_id;
    QString source_path;

    bool operator==(const ReviewLocalSourceProbe&) const = default;
};

struct ReviewLocalSourceAvailability final {
    ReviewLocalSourceProbe source;
    bool available = false;

    bool operator==(const ReviewLocalSourceAvailability&) const = default;
};

struct ReviewItem final {
    QString photo_id;
    QString representation_id;
    std::uint32_t representation_count = 1;
    std::uint32_t source_location_count = 1;
    bool has_raw_representation = false;
    bool has_raster_representation = false;
    QString location_id;
    QString visual_handle;
    /// Direct `file:` URL for a verified client-local remote proxy. Local
    /// Catalog rows continue to use the authenticated image-provider handle.
    QString visual_source_override;
    bool is_remote = false;
    /// The authoritative original remains remote even when a verified local
    /// cache object can satisfy editing without contacting its server.
    bool remote_original_cached = false;
    /// Stable client connection identity. Presentation uses this to resolve
    /// the status of the exact server that owns the remote representation.
    QString remote_connection_id;
    QString remote_server_id;
    QString remote_photo_id;
    QString remote_representation_id;
    QString remote_preview_unavailable_reason;
    quint64 decision_head_sequence = 0;
    QString decision_flag = QStringLiteral("unflagged");
    int decision_rating = 0;
    /// Durable Library state lives in the Catalog, not in desktop settings.
    bool liked = false;
    QString color_label = QStringLiteral("none");
    std::int64_t library_state_updated_at_ms = 0;
    bool has_development_edits = false;
    QString title;
    QString source_path;
    bool source_available = true;
    QString visual_role;
    std::uint32_t visual_width = 0;
    std::uint32_t visual_height = 0;
    bool has_visual = false;
    bool has_metadata = false;
    QString camera_make;
    QString camera_model;
    QString lens_make;
    QString lens_model;
    std::int64_t captured_at_unix_seconds = 0;
    double iso_speed = 0.0;
    double exposure_time_seconds = 0.0;
    double aperture_f_number = 0.0;
    double focal_length_mm = 0.0;
    double focal_length_35mm = 0.0;
    std::uint32_t raw_width = 0;
    std::uint32_t raw_height = 0;
    std::uint32_t sensor_bits = 0;
    QString cfa_pattern;
    QString dng_version;
    bool has_technical_observation = false;
    std::uint32_t technical_input_width = 0;
    std::uint32_t technical_input_height = 0;
    QString technical_preprocessing_version;
    QString technical_implementation_version;
    double mean_luma = 0.0;
    double p01_luma = 0.0;
    double p50_luma = 0.0;
    double p99_luma = 0.0;
    double near_black_fraction = 0.0;
    double near_white_fraction = 0.0;
    double laplacian_variance = 0.0;
    double edge_energy = 0.0;
};

class ReviewModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role {
        PhotoIdRole = Qt::UserRole + 1,
        RepresentationIdRole,
        RepresentationCountRole,
        SourceLocationCountRole,
        HasRawRepresentationRole,
        HasRasterRepresentationRole,
        LocationIdRole,
        VisualHandleRole,
        TitleRole,
        SourcePathRole,
        SourceAvailableRole,
        VisualRole,
        VisualErrorRole,
        VisualWidthRole,
        VisualHeightRole,
        VisualSourceRole,
        HasMetadataRole,
        CameraMakeRole,
        CameraModelRole,
        LensMakeRole,
        LensModelRole,
        CapturedAtUnixSecondsRole,
        IsoSpeedRole,
        ExposureTimeSecondsRole,
        ApertureFNumberRole,
        FocalLengthMmRole,
        FocalLength35mmRole,
        RawWidthRole,
        RawHeightRole,
        SensorBitsRole,
        CfaPatternRole,
        DngVersionRole,
        HasTechnicalObservationRole,
        TechnicalInputWidthRole,
        TechnicalInputHeightRole,
        TechnicalPreprocessingVersionRole,
        TechnicalImplementationVersionRole,
        MeanLumaRole,
        P01LumaRole,
        P50LumaRole,
        P99LumaRole,
        NearBlackFractionRole,
        NearWhiteFractionRole,
        LaplacianVarianceRole,
        EdgeEnergyRole,
        DecisionHeadSequenceRole,
        DecisionFlagRole,
        DecisionRatingRole,
        LikedRole,
        ColorLabelRole,
        LibraryStateUpdatedAtMsRole,
        HasDevelopmentEditsRole,
        IsRemoteRole,
        RemoteOriginalCachedRole,
        RemoteConnectionIdRole,
        RemoteServerIdRole,
        RemotePhotoIdRole,
        RemoteRepresentationIdRole,
        RemotePreviewUnavailableReasonRole,
    };
    Q_ENUM(Role)

    explicit ReviewModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(QVector<ReviewItem> items, quint64 generation);
    /// Advances the request generation while retaining the current visible
    /// rows until the next photo-first page reconciles them. Retained visual
    /// roles are republished because their provider URLs carry this generation.
    void setGeneration(quint64 generation);
    void append(QVector<ReviewItem> items);
    // Appends only a current-generation page whose stable keys are unique both
    // within the page and across the already presented rows.
    [[nodiscard]] bool appendSnapshot(QVector<ReviewItem> items, quint64 generation);
    // Applies only a current-generation snapshot with unique, non-empty stable keys.
    [[nodiscard]] bool reconcileSnapshot(QVector<ReviewItem> items, quint64 generation);
    // Reconciles an ordered from-origin prefix while retaining every existing
    // row whose key is not present in that prefix. This is the live-import
    // operation: no unstable pagination cursor is exposed while the Catalog is
    // still changing.
    [[nodiscard]] bool reconcilePrefixSnapshot(QVector<ReviewItem> items, quint64 generation);
    /// Replaces only client-mirrored remote rows while preserving every local
    /// Catalog row and the current query generation.
    [[nodiscard]] bool replaceRemoteItems(QVector<ReviewItem> items);
    enum class PresentationSortKey : std::uint8_t { CaptureTime, Name };
    /// Orders the merged local/remote presentation. Catalog pagination remains
    /// source-local; this model owns the final cross-source ordering.
    void setPresentationOrder(PresentationSortKey key, bool descending);
    [[nodiscard]] bool isGenerationCurrent(quint64 generation) const noexcept;
    /// Builds a generation-bound comparison source. Grid roles use their own
    /// immutable lifetime contract when projected through data().
    [[nodiscard]] QString visualSourceFor(const QString& ticket) const;
    [[nodiscard]] std::optional<ReviewDecisionValue> decisionFor(const QString& photo_id) const;
    [[nodiscard]] std::optional<ReviewLibraryStateValue>
    libraryStateFor(const QString& photo_id) const;
    /// Snapshots every currently loaded local source for off-thread file
    /// availability checks. Remote rows have a separate materialization
    /// lifecycle and are intentionally excluded.
    [[nodiscard]] QVector<ReviewLocalSourceProbe> localSourceProbes() const;
    /// Applies exact-location observations without replacing rows or changing
    /// the query generation. Stale results are ignored after relink/reset.
    [[nodiscard]] bool
    applyLocalSourceAvailability(const QVector<ReviewLocalSourceAvailability>& observations);
    [[nodiscard]] bool
    updateDecision(const QString& photo_id, quint64 head_sequence, const QString& flag, int rating);
    /// Applies the Catalog-authoritative durable Library state for a loaded
    /// photo. The stable model key is photo_id; representation_id may change
    /// when a source is relinked without creating a second logical photo.
    [[nodiscard]] bool updateLibraryState(
        const QString& photo_id,
        bool liked,
        const QString& color_label,
        std::int64_t updated_at_ms
    );

  signals:
    void localSourceAvailabilityChanged(const QString& photoId, bool available);

  private:
    QVector<ReviewItem> items_;
    std::atomic<quint64> generation_ = 0;
    PresentationSortKey presentation_sort_key_ = PresentationSortKey::CaptureTime;
    bool presentation_sort_descending_ = true;
};

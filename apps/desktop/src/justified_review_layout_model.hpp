#pragma once

#include <QAbstractListModel>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

/// Presents a flat photo model as virtualized, Lightroom-style justified rows.
///
/// The source model stays authoritative for photo data and filtering. This
/// adapter only calculates row geometry from cached visual dimensions, so it
/// never waits for an Image element to load before placing a photo. A ListView
/// can therefore virtualize whole rows even in a large catalog.
class JustifiedReviewLayoutModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(
        QAbstractItemModel* sourceModel READ sourceModel WRITE setSourceModel NOTIFY
            sourceModelChanged
    )
    Q_PROPERTY(int availableWidth READ availableWidth WRITE setAvailableWidth NOTIFY layoutChanged)
    Q_PROPERTY(
        int targetRowHeight READ targetRowHeight WRITE setTargetRowHeight NOTIFY layoutChanged
    )
    Q_PROPERTY(int spacing READ spacing WRITE setSpacing NOTIFY layoutChanged)
    Q_PROPERTY(QVariantList sections READ sections WRITE setSections NOTIFY sectionsChanged)
    Q_PROPERTY(QVariantList sectionAnchors READ sectionAnchors NOTIFY sectionAnchorsChanged)

  public:
    enum Role {
        ItemsRole = Qt::UserRole + 1,
        RowHeightRole,
        UsedWidthRole,
        RowKindRole,
        SectionKeyRole,
        SectionTitleRole,
        SectionSubtitleRole,
        SectionItemCountRole,
        SectionOrdinalRole,
    };
    Q_ENUM(Role)

    explicit JustifiedReviewLayoutModel(QObject* parent = nullptr);
    ~JustifiedReviewLayoutModel() override;

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QAbstractItemModel* sourceModel() const noexcept;
    void setSourceModel(QAbstractItemModel* source_model);

    [[nodiscard]] int availableWidth() const noexcept;
    void setAvailableWidth(int width);

    [[nodiscard]] int targetRowHeight() const noexcept;
    void setTargetRowHeight(int height);

    [[nodiscard]] int spacing() const noexcept;
    void setSpacing(int spacing);

    /// Applies an ordered presentation-only partition to the already filtered
    /// source model. Each descriptor accepts `key`, `title`, `subtitle`,
    /// optional navigation labels, and exact `{photo, representation}`
    /// identities in `representationKeys`.
    /// Unassigned photos remain visible after the named sections, so grouping
    /// can never silently widen or narrow the active Library filters.
    [[nodiscard]] QVariantList sections() const;
    void setSections(const QVariantList& sections);

    /// Exposes the concrete layout-row index for every visible section header.
    /// Grouping semantics remain upstream; this projection only translates
    /// ordered section descriptors into ListView jump anchors.
    [[nodiscard]] QVariantList sectionAnchors() const;

    /// Resolves spatial keyboard navigation against the virtualized justified
    /// rows. Horizontal movement follows catalog order; vertical movement
    /// chooses the nearest photo center in the adjacent visual row.
    Q_INVOKABLE QVariantMap navigationTarget(
        const QString& photo_id,
        const QString& representation_id,
        int horizontal_delta,
        int vertical_delta
    ) const;

  signals:
    void sourceModelChanged();
    void layoutChanged();
    void sectionsChanged();
    void sectionAnchorsChanged();

  private:
    struct Row final {
        QString kind = QStringLiteral("photos");
        QVariantList items;
        int height = 0;
        int used_width = 0;
        QString section_key;
        QString section_title;
        QString section_subtitle;
        QString navigation_label;
        QString navigation_short_label;
        QString navigation_major_label;
        int section_item_count = 0;
        int section_ordinal = -1;
    };

    struct Section final {
        QString key;
        QString title;
        QString subtitle;
        QString navigation_label;
        QString navigation_short_label;
        QString navigation_major_label;
        QStringList representation_keys;
    };

    [[nodiscard]] QVariantMap sourceItem(int row) const;
    [[nodiscard]] static qreal aspectRatio(const QVariantMap& item);
    void disconnectSourceModel();
    void requestRebuild();
    void
    updateSourceItems(const QModelIndex& first, const QModelIndex& last, const QList<int>& roles);
    void rebuild();
    void appendPhotoRows(QVector<Row>& rows, QVariantList items) const;

    QPointer<QAbstractItemModel> source_model_;
    QVector<QMetaObject::Connection> source_connections_;
    QTimer rebuild_timer_;
    QVector<Row> rows_;
    QHash<QString, QPair<int, int>> item_positions_;
    QVariantList section_variants_;
    QVector<Section> sections_;
    int available_width_ = 0;
    int target_row_height_ = 188;
    int spacing_ = 8;
};

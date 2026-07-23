#pragma once

#include <QAbstractListModel>
#include <QMetaObject>
#include <QPointer>
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
        QAbstractItemModel* sourceModel
        READ sourceModel
        WRITE setSourceModel
        NOTIFY sourceModelChanged
    )
    Q_PROPERTY(
        int availableWidth
        READ availableWidth
        WRITE setAvailableWidth
        NOTIFY layoutChanged
    )
    Q_PROPERTY(
        int targetRowHeight
        READ targetRowHeight
        WRITE setTargetRowHeight
        NOTIFY layoutChanged
    )
    Q_PROPERTY(
        int spacing
        READ spacing
        WRITE setSpacing
        NOTIFY layoutChanged
    )

public:
    enum Role {
        ItemsRole = Qt::UserRole + 1,
        RowHeightRole,
        UsedWidthRole,
    };
    Q_ENUM(Role)

    explicit JustifiedReviewLayoutModel(QObject* parent = nullptr);
    ~JustifiedReviewLayoutModel() override;

    [[nodiscard]] int rowCount(
        const QModelIndex& parent = QModelIndex()
    ) const override;
    [[nodiscard]] QVariant data(
        const QModelIndex& index,
        int role
    ) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QAbstractItemModel* sourceModel() const noexcept;
    void setSourceModel(QAbstractItemModel* source_model);

    [[nodiscard]] int availableWidth() const noexcept;
    void setAvailableWidth(int width);

    [[nodiscard]] int targetRowHeight() const noexcept;
    void setTargetRowHeight(int height);

    [[nodiscard]] int spacing() const noexcept;
    void setSpacing(int spacing);

signals:
    void sourceModelChanged();
    void layoutChanged();

private:
    struct Row final {
        QVariantList items;
        int height = 0;
        int used_width = 0;
    };

    [[nodiscard]] QVariantMap sourceItem(int row) const;
    [[nodiscard]] static qreal aspectRatio(const QVariantMap& item);
    void disconnectSourceModel();
    void rebuild();

    QPointer<QAbstractItemModel> source_model_;
    QVector<QMetaObject::Connection> source_connections_;
    QVector<Row> rows_;
    int available_width_ = 0;
    int target_row_height_ = 188;
    int spacing_ = 8;
};

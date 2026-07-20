#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QReadWriteLock>
#include <QString>
#include <QVector>

struct ReviewItem final {
    QString photo_id;
    QString representation_id;
    QString title;
    QString source_path;
    QString visual_role;
    QString visual_error;
    std::uint32_t visual_width = 0;
    std::uint32_t visual_height = 0;
    QByteArray visual_bytes;
};

class ReviewModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        PhotoIdRole = Qt::UserRole + 1,
        RepresentationIdRole,
        TitleRole,
        SourcePathRole,
        VisualRole,
        VisualErrorRole,
        VisualWidthRole,
        VisualHeightRole,
        VisualSourceRole,
    };
    Q_ENUM(Role)

    explicit ReviewModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(QVector<ReviewItem> items);
    [[nodiscard]] QByteArray visualBytes(const QString& representation_id) const;

private:
    mutable QReadWriteLock lock_;
    QVector<ReviewItem> items_;
    QHash<QString, qsizetype> row_by_id_;
    quint64 generation_ = 0;
};

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVector>

#include <atomic>

struct ReviewItem final {
    QString photo_id;
    QString representation_id;
    QString title;
    QString source_path;
    QString visual_role;
    std::uint32_t visual_width = 0;
    std::uint32_t visual_height = 0;
    bool has_visual = false;
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

    void replace(QVector<ReviewItem> items, quint64 generation);
    void append(QVector<ReviewItem> items);
    [[nodiscard]] bool isGenerationCurrent(quint64 generation) const noexcept;

private:
    QVector<ReviewItem> items_;
    std::atomic<quint64> generation_ = 0;
};

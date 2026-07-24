#pragma once

#include <QSortFilterProxyModel>

/// A client-side Lightroom-style library filter over the locally loaded page.
/// The catalog remains the source of truth; filtering only controls what the
/// Review grid presents and can therefore remain responsive while import is
/// still streaming more items.
class ReviewFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(
        QString flagFilter
        READ flagFilter
        WRITE setFlagFilter
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        int minimumRating
        READ minimumRating
        WRITE setMinimumRating
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString colorFilter
        READ colorFilter
        WRITE setColorFilter
        NOTIFY filtersChanged
    )
    Q_PROPERTY(
        QString editFilter
        READ editFilter
        WRITE setEditFilter
        NOTIFY filtersChanged
    )

public:
    explicit ReviewFilterModel(QObject* parent = nullptr);

    [[nodiscard]] QString flagFilter() const;
    [[nodiscard]] int minimumRating() const noexcept;
    [[nodiscard]] QString colorFilter() const;
    [[nodiscard]] QString editFilter() const;

    void setFlagFilter(const QString& filter);
    void setMinimumRating(int rating);
    void setColorFilter(const QString& filter);
    void setEditFilter(const QString& filter);
    Q_INVOKABLE void clearFilters();

signals:
    void filtersChanged();

protected:
    [[nodiscard]] bool filterAcceptsRow(
        int source_row,
        const QModelIndex& source_parent
    ) const override;

private:
    [[nodiscard]] static QString normalizeFlagFilter(const QString& filter);
    [[nodiscard]] static QString normalizeColorFilter(const QString& filter);
    [[nodiscard]] static QString normalizeEditFilter(const QString& filter);
    void refreshRowsFilter();

    QString flag_filter_ = QStringLiteral("all");
    int minimum_rating_ = 0;
    QString color_filter_ = QStringLiteral("all");
    QString edit_filter_ = QStringLiteral("all");
};

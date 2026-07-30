#pragma once

#include <QSortFilterProxyModel>
#include <QStringList>

/// A client-side Lightroom-style library filter over the locally loaded page.
/// The catalog remains the source of truth; filtering only controls what the
/// Review grid presents and can therefore remain responsive while import is
/// still streaming more items.
class ReviewFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString flagFilter READ flagFilter WRITE setFlagFilter NOTIFY filtersChanged)
    Q_PROPERTY(int minimumRating READ minimumRating WRITE setMinimumRating NOTIFY filtersChanged)
    Q_PROPERTY(QString colorFilter READ colorFilter WRITE setColorFilter NOTIFY filtersChanged)
    Q_PROPERTY(QString editFilter READ editFilter WRITE setEditFilter NOTIFY filtersChanged)
    Q_PROPERTY(QString likedFilter READ likedFilter WRITE setLikedFilter NOTIFY filtersChanged)
    Q_PROPERTY(
        QString excludedFlagFilter READ excludedFlagFilter WRITE setExcludedFlagFilter NOTIFY
            filtersChanged
    )
    Q_PROPERTY(
        QString excludedColorFilter READ excludedColorFilter WRITE setExcludedColorFilter NOTIFY
            filtersChanged
    )

  public:
    explicit ReviewFilterModel(QObject* parent = nullptr);

    [[nodiscard]] QString flagFilter() const;
    [[nodiscard]] int minimumRating() const noexcept;
    [[nodiscard]] QString colorFilter() const;
    [[nodiscard]] QString editFilter() const;
    [[nodiscard]] QString likedFilter() const;
    [[nodiscard]] QString excludedFlagFilter() const;
    [[nodiscard]] QString excludedColorFilter() const;
    [[nodiscard]] QString captureMonth() const;
    [[nodiscard]] QString cameraKey() const;
    [[nodiscard]] QString lensKey() const;
    [[nodiscard]] QStringList keywordIdsAll() const;
    [[nodiscard]] QStringList excludedKeywordIdsAny() const;
    [[nodiscard]] bool hasActiveServerFilter() const;

    void setFlagFilter(const QString& filter);
    void setMinimumRating(int rating);
    void setColorFilter(const QString& filter);
    void setEditFilter(const QString& filter);
    void setLikedFilter(const QString& filter);
    void setExcludedFlagFilter(const QString& filter);
    void setExcludedColorFilter(const QString& filter);
    /// Metadata facets are catalog-side values. They intentionally do not
    /// attempt a lossy make/model comparison over the currently retained grid
    /// page; the next photo-first page is the authoritative result.
    void setCaptureMonth(const QString& capture_month);
    void setCameraKey(const QString& camera_key);
    void setLensKey(const QString& lens_key);
    void setKeywordIdsAll(const QStringList& keyword_ids);
    void setExcludedKeywordIdsAny(const QStringList& keyword_ids);
    Q_INVOKABLE void clearFilters();

  signals:
    void filtersChanged();

  protected:
    [[nodiscard]] bool
    filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

  private:
    [[nodiscard]] static QString normalizeFlagFilter(const QString& filter);
    [[nodiscard]] static QString normalizeColorFilter(const QString& filter);
    [[nodiscard]] static QString normalizeEditFilter(const QString& filter);
    [[nodiscard]] static QString normalizeLikedFilter(const QString& filter);
    [[nodiscard]] static QString normalizeCaptureMonth(const QString& value);
    [[nodiscard]] static QString normalizeFacetKey(const QString& value);
    [[nodiscard]] static QStringList normalizeKeywordIds(const QStringList& values);
    void refreshRowsFilter();

    QString flag_filter_ = QStringLiteral("all");
    int minimum_rating_ = 0;
    QString color_filter_ = QStringLiteral("all");
    QString edit_filter_ = QStringLiteral("all");
    QString liked_filter_ = QStringLiteral("all");
    QString excluded_flag_filter_ = QStringLiteral("all");
    QString excluded_color_filter_ = QStringLiteral("all");
    QString capture_month_;
    QString camera_key_;
    QString lens_key_;
    QStringList keyword_ids_all_;
    QStringList excluded_keyword_ids_any_;
};

#pragma once

#include <QHash>
#include <QSet>
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
    [[nodiscard]] bool hideOfflineUncached() const noexcept;
    [[nodiscard]] bool onlyEditable() const noexcept;
    [[nodiscard]] QString excludedFlagFilter() const;
    [[nodiscard]] QString excludedColorFilter() const;
    [[nodiscard]] QString captureMonth() const;
    [[nodiscard]] int chineseLunarMonth() const noexcept;
    [[nodiscard]] int chineseLunarDay() const noexcept;
    [[nodiscard]] QString chineseLunarMonthType() const;
    [[nodiscard]] QString cameraKey() const;
    [[nodiscard]] QString lensKey() const;
    [[nodiscard]] QString countryKey() const;
    [[nodiscard]] QString localityKey() const;
    [[nodiscard]] bool travelFilterEnabled() const noexcept;
    [[nodiscard]] bool dailyFilterEnabled() const noexcept;
    [[nodiscard]] QStringList keywordIdsAll() const;
    [[nodiscard]] QStringList excludedKeywordIdsAny() const;
    [[nodiscard]] bool hasActiveServerFilter() const;
    [[nodiscard]] bool semanticFilterActive() const noexcept;
    [[nodiscard]] bool smartCategoryFilterActive() const noexcept;

    void setFlagFilter(const QString& filter);
    void setMinimumRating(int rating);
    void setColorFilter(const QString& filter);
    void setEditFilter(const QString& filter);
    void setLikedFilter(const QString& filter);
    void setHideOfflineUncached(bool enabled);
    void setOnlyEditable(bool enabled);
    void setExcludedFlagFilter(const QString& filter);
    void setExcludedColorFilter(const QString& filter);
    /// Metadata facets are catalog-side values. They intentionally do not
    /// attempt a lossy make/model comparison over the currently retained grid
    /// page; the next photo-first page is the authoritative result.
    void setCaptureMonth(const QString& capture_month);
    void setChineseLunarMonth(int month);
    void setChineseLunarDay(int day);
    void setChineseLunarMonthType(const QString& month_type);
    void setCameraKey(const QString& camera_key);
    void setLensKey(const QString& lens_key);
    void setCountryKey(const QString& country_key);
    void setLocalityKey(const QString& locality_key);
    void setTravelFilterEnabled(bool enabled);
    void setDailyFilterEnabled(bool enabled);
    void setKeywordIdsAll(const QStringList& keyword_ids);
    void setExcludedKeywordIdsAny(const QStringList& keyword_ids);
    /// Restricts the loaded Review model to exact semantic matches and orders
    /// them from strongest to weakest. Keys use the controller-owned
    /// `{photo, representation}` identity grammar.
    void setSemanticRepresentationOrder(const QStringList& ranked_keys);
    /// Restricts the loaded Review model to exact members of one published
    /// smart category. Unlike semantic search this filter preserves grid order.
    void setSmartCategoryRepresentationKeys(const QStringList& member_keys);
    Q_INVOKABLE void clearFilters();

  signals:
    void filtersChanged();
    void availabilityFiltersChanged();
    void semanticFilterChanged();
    void smartCategoryFilterChanged();

  protected:
    [[nodiscard]] bool
    filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;
    [[nodiscard]] bool
    lessThan(const QModelIndex& source_left, const QModelIndex& source_right) const override;

  private:
    [[nodiscard]] static QString normalizeFlagFilter(const QString& filter);
    [[nodiscard]] static QString normalizeColorFilter(const QString& filter);
    [[nodiscard]] static QString normalizeEditFilter(const QString& filter);
    [[nodiscard]] static QString normalizeLikedFilter(const QString& filter);
    [[nodiscard]] static QString normalizeCaptureMonth(const QString& value);
    [[nodiscard]] static QString normalizeChineseLunarMonthType(const QString& value);
    [[nodiscard]] static QString normalizeFacetKey(const QString& value);
    [[nodiscard]] static QStringList normalizeKeywordIds(const QStringList& values);
    void refreshRowsFilter();

    QString flag_filter_ = QStringLiteral("all");
    int minimum_rating_ = 0;
    QString color_filter_ = QStringLiteral("all");
    QString edit_filter_ = QStringLiteral("all");
    QString liked_filter_ = QStringLiteral("all");
    bool hide_offline_uncached_ = false;
    bool only_editable_ = false;
    QString excluded_flag_filter_ = QStringLiteral("all");
    QString excluded_color_filter_ = QStringLiteral("all");
    QString capture_month_;
    int chinese_lunar_month_ = 0;
    int chinese_lunar_day_ = 0;
    QString chinese_lunar_month_type_ = QStringLiteral("all");
    QString camera_key_;
    QString lens_key_;
    QString country_key_;
    QString locality_key_;
    bool travel_filter_enabled_ = false;
    bool daily_filter_enabled_ = false;
    QStringList keyword_ids_all_;
    QStringList excluded_keyword_ids_any_;
    QHash<QString, qsizetype> semantic_rank_by_key_;
    QSet<QString> smart_category_keys_;
};

#pragma once

#include <QAbstractItemModel>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVariantList>

/// Compiles presentation-only gallery groups from the currently filtered photo model.
///
/// Dimensions use stable keys so the toolbar can render the registry generically. Date
/// granularities are mutually exclusive; independent dimensions compose in registry order.
/// Semantic relevance sections remain an optional outer grouping supplied by the search owner.
class ReviewGalleryGroupingController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(
        QAbstractItemModel* sourceModel READ sourceModel WRITE setSourceModel NOTIFY
            sourceModelChanged
    )
    Q_PROPERTY(
        QVariantList baseSections READ baseSections WRITE setBaseSections NOTIFY baseSectionsChanged
    )
    Q_PROPERTY(QVariantList dimensions READ dimensions NOTIFY groupingChanged)
    Q_PROPERTY(QVariantList sections READ sections NOTIFY sectionsChanged)
    Q_PROPERTY(int activeDimensionCount READ activeDimensionCount NOTIFY groupingChanged)
    Q_PROPERTY(QString activeSummary READ activeSummary NOTIFY groupingChanged)

  public:
    explicit ReviewGalleryGroupingController(QObject* parent = nullptr);
    ~ReviewGalleryGroupingController() override;

    [[nodiscard]] QAbstractItemModel* sourceModel() const noexcept;
    void setSourceModel(QAbstractItemModel* source_model);

    [[nodiscard]] QVariantList baseSections() const;
    void setBaseSections(const QVariantList& sections);

    [[nodiscard]] QVariantList dimensions() const;
    [[nodiscard]] QVariantList sections() const;
    [[nodiscard]] int activeDimensionCount() const noexcept;
    [[nodiscard]] QString activeSummary() const;

    Q_INVOKABLE void setDimensionSelected(const QString& key, bool selected);
    Q_INVOKABLE void clearGrouping();
    Q_INVOKABLE void retranslateUi();

  signals:
    void sourceModelChanged();
    void baseSectionsChanged();
    void groupingChanged();
    void sectionsChanged();

  private:
    void disconnectSourceModel();
    void rebuild();

    QPointer<QAbstractItemModel> source_model_;
    QList<QMetaObject::Connection> source_connections_;
    QVariantList base_sections_;
    QVariantList sections_;
    QSet<QString> selected_dimensions_;
};

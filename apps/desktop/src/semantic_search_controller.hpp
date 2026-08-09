#pragma once

#include "backend/semantic_search_types.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QVariantList>

#include <functional>

struct SemanticSearchTaskResult final {
    BackendSemanticSearchReport report;
    QString query;
    QString diagnostic;
};

/// Owns manual, bounded semantic-search admission and its session projection.
/// One request runs at a time; destruction waits for the worker so captured
/// backend/session state cannot outlive the desktop composition root.
class SemanticSearchController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool hasResults READ hasResults NOTIFY resultsChanged)
    Q_PROPERTY(QString activeQuery READ activeQuery NOTIFY resultsChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QVariantList matches READ matches NOTIFY resultsChanged)
    Q_PROPERTY(
        QStringList rankedRepresentationKeys READ rankedRepresentationKeys NOTIFY resultsChanged
    )
    Q_PROPERTY(
        QString relevanceFilter READ relevanceFilter WRITE setRelevanceFilter NOTIFY resultsChanged
    )
    Q_PROPERTY(uint highRelevanceCount READ highRelevanceCount NOTIFY resultsChanged)
    Q_PROPERTY(uint possibleRelevanceCount READ possibleRelevanceCount NOTIFY resultsChanged)
    Q_PROPERTY(uint hiddenLowRelevanceCount READ hiddenLowRelevanceCount NOTIFY resultsChanged)
    Q_PROPERTY(uint shownResultCount READ shownResultCount NOTIFY resultsChanged)
    Q_PROPERTY(uint consideredPhotos READ consideredPhotos NOTIFY resultsChanged)
    Q_PROPERTY(uint embeddedPhotos READ embeddedPhotos NOTIFY resultsChanged)
    Q_PROPERTY(uint skippedItems READ skippedItems NOTIFY resultsChanged)
    Q_PROPERTY(bool truncated READ truncated NOTIFY resultsChanged)

  public:
    using Runner =
        std::function<BackendSemanticSearchReport(const QString&, const QString&, const QString&)>;

    explicit SemanticSearchController(Runner runner, QObject* parent = nullptr);
    ~SemanticSearchController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool hasResults() const noexcept;
    [[nodiscard]] QString activeQuery() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QVariantList matches() const;
    [[nodiscard]] QStringList rankedRepresentationKeys() const;
    [[nodiscard]] QString relevanceFilter() const;
    [[nodiscard]] uint highRelevanceCount() const noexcept;
    [[nodiscard]] uint possibleRelevanceCount() const noexcept;
    [[nodiscard]] uint hiddenLowRelevanceCount() const noexcept;
    [[nodiscard]] uint shownResultCount() const noexcept;
    [[nodiscard]] uint consideredPhotos() const noexcept;
    [[nodiscard]] uint embeddedPhotos() const noexcept;
    [[nodiscard]] uint skippedItems() const noexcept;
    [[nodiscard]] bool truncated() const noexcept;

    Q_INVOKABLE void search(const QString& query);
    Q_INVOKABLE void setRelevanceFilter(const QString& filter);
    Q_INVOKABLE void clearSessionResults();
    Q_INVOKABLE void retranslateUi();

  signals:
    void stateChanged();
    void resultsChanged();

  private:
    enum class State {
        Idle,
        Running,
        Ready,
        Failed,
    };

    void finishSearch();

    Runner runner_;
    QFutureWatcher<SemanticSearchTaskResult> watcher_;
    BackendSemanticSearchReport report_;
    QString active_query_;
    QString relevance_filter_ = QStringLiteral("all");
    State state_ = State::Idle;
    bool has_results_ = false;
};

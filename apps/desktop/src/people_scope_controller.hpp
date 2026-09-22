#pragma once

#include "backend/library_types.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>

class PeopleAnalysisController;

/// An immutable Review lens. Person membership is deliberately absent so a
/// return from one person still shows the original album and filter scope.
struct PeopleScopeSnapshot final {
    BackendLibraryPhotoFilter filter;
    bool active = false;
    bool only_editable = false;
    QString excluded_flag = QStringLiteral("all");
    QString excluded_color = QStringLiteral("all");
    QStringList semantic_keys;
    QStringList smart_category_keys;
};

struct PeopleScopeResult final {
    QVariantMap counts;
    QString diagnostic;
    bool cancelled = false;
};

/// Projects durable, global People groups through one Review scope. Catalog
/// queries run on a worker; changing the lens invalidates its old result.
class PeopleScopeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)

  public:
    using Count = std::function<std::uint64_t(const BackendLibraryPhotoFilter&)>;
    using Page = std::function<BackendLibraryPhotoPage(
        const BackendLibraryPhotoFilter&,
        const BackendLibraryPhotoCursor&,
        std::uint32_t
    )>;
    using Snapshot = std::function<PeopleScopeSnapshot()>;

    PeopleScopeController(
        Count count,
        Page page,
        Snapshot snapshot,
        PeopleAnalysisController* people,
        QObject* parent = nullptr
    );
    ~PeopleScopeController() override;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] QVariantMap counts() const;
    [[nodiscard]] QString errorText() const;

    Q_INVOKABLE void setViewActive(bool active);
    Q_INVOKABLE void invalidate();

  signals:
    void stateChanged();

  private:
    void start();
    void finish();

    Count count_;
    Page page_;
    Snapshot snapshot_;
    PeopleAnalysisController* people_ = nullptr;
    QFutureWatcher<PeopleScopeResult> watcher_;
    QTimer debounce_;
    std::shared_ptr<std::atomic_bool> cancel_;
    QVariantMap counts_;
    QString error_text_;
    bool view_active_ = false;
    bool active_ = false;
    bool ready_ = false;
    bool dirty_ = false;
};

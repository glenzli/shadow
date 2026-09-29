#pragma once

#include "backend/library_types.hpp"
#include "backend/semantic_search_types.hpp"

#include <QAbstractItemModel>
#include <QFutureWatcher>
#include <QObject>
#include <QVariantList>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

struct ReviewClusteringResult final {
    QVector<QStringList> groups;
    QString diagnostic;
    bool cancelled = false;
    bool truncated = false;
    int total = 0;
};

/// Session-only, bounded candidate grouping. A worker owns the immutable scope,
/// provider calls and cooperative pause point; the GUI thread owns projection.
class ReviewClusteringController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY stateChanged)
    Q_PROPERTY(bool canResume READ canResume NOTIFY stateChanged)
    Q_PROPERTY(bool hasResults READ hasResults NOTIFY stateChanged)
    Q_PROPERTY(bool hasStatus READ hasStatus NOTIFY stateChanged)
    Q_PROPERTY(bool cancelling READ cancelling NOTIFY stateChanged)
    Q_PROPERTY(int processed READ processed NOTIFY stateChanged)
    Q_PROPERTY(int total READ total NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QVariantList groups READ groups NOTIFY stateChanged)

  public:
    using ImageRunner =
        std::function<BackendSemanticSearchReport(const QString&, const QString&, std::uint64_t)>;
    using Begin = std::function<std::uint64_t()>;
    using Cancel = std::function<void(std::uint64_t)>;
    using AlbumPage = std::function<
        BackendLibraryPhotoPage(const QString&, const BackendLibraryPhotoCursor&, std::uint32_t)>;

    ReviewClusteringController(
        ImageRunner image_runner,
        Begin begin,
        Cancel cancel,
        AlbumPage album_page,
        QObject* parent = nullptr
    );
    ~ReviewClusteringController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] bool canResume() const noexcept;
    [[nodiscard]] bool hasResults() const noexcept;
    [[nodiscard]] bool hasStatus() const noexcept;
    [[nodiscard]] bool cancelling() const noexcept;
    [[nodiscard]] int processed() const noexcept;
    [[nodiscard]] int total() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QVariantList groups() const;

    Q_INVOKABLE bool startSelected(const QVariantList& targets);
    Q_INVOKABLE bool startView(QAbstractItemModel* model);
    Q_INVOKABLE bool startAlbum(const QString& album_id);
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void retranslateUi();

  signals:
    void stateChanged();

  private:
    struct Ticket final {
        QString photo_id;
        QString representation_id;
    };
    enum class State {
        Idle,
        Invalid,
        Collecting,
        Running,
        Pausing,
        Paused,
        Cancelling,
        Ready,
        Cancelled,
        Failed
    };

    static QString key(const Ticket& ticket);
    bool start(QVector<Ticket> tickets, const QString& album_id);
    ReviewClusteringResult
    run(QVector<Ticket> tickets, const QString& album_id, std::uint64_t generation);
    bool waitIfPaused(std::uint64_t generation);
    void publishProgress(std::uint64_t generation, int processed, int total);
    void finish();

    ImageRunner image_runner_;
    Begin begin_;
    Cancel cancel_;
    AlbumPage album_page_;
    QFutureWatcher<ReviewClusteringResult> watcher_;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> pause_requested_{false};
    std::atomic<std::uint64_t> active_token_{0};
    std::mutex pause_mutex_;
    std::condition_variable pause_condition_;
    std::uint64_t generation_ = 0;
    State state_ = State::Idle;
    int processed_ = 0;
    int total_ = 0;
    bool truncated_ = false;
    bool discard_result_ = false;
    QVector<QStringList> groups_;
};

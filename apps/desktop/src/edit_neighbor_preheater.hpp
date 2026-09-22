#pragma once

#include "desktop_backend.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>

#include <atomic>
#include <memory>

class EditController;
class JustifiedReviewLayoutModel;

// The Review model owns ordering; this object owns only one cancellable,
// invisible preparation of the next locally available edit source.
class EditNeighborPreheater final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap nextTarget READ nextTarget NOTIFY targetsChanged)
    Q_PROPERTY(QVariantMap previousTarget READ previousTarget NOTIFY targetsChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool preparing READ preparing NOTIFY stateChanged)
    Q_PROPERTY(bool prepared READ prepared NOTIFY stateChanged)
    Q_PROPERTY(bool atLoadedEnd READ atLoadedEnd NOTIFY targetsChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

  public:
    enum class State { Unavailable, Waiting, Preparing, Prepared, Failed };
    Q_ENUM(State)

    EditNeighborPreheater(
        std::shared_ptr<DesktopBackend> backend,
        EditController& editor,
        JustifiedReviewLayoutModel& navigation,
        QObject* parent = nullptr
    );
    ~EditNeighborPreheater() override;

    [[nodiscard]] QVariantMap nextTarget() const;
    [[nodiscard]] QVariantMap previousTarget() const;
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool preparing() const noexcept;
    [[nodiscard]] bool prepared() const noexcept;
    [[nodiscard]] bool atLoadedEnd() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    void setEnabled(bool enabled);

  signals:
    void targetsChanged();
    void stateChanged();
    void enabledChanged();

  private:
    struct WorkResult {
        QString photo_id;
        QString representation_id;
        QString source_path;
        QString error;
        bool completed = false;
        bool cancelled = false;
        qint64 elapsed_ms = 0;
    };

    void refreshTargets();
    void updateAdmission();
    void startIfIdle();
    void finishWork();
    void cancelWork();
    void setState(State state);
    void updateDetailPriority();
    [[nodiscard]] bool canPrepare() const;
    [[nodiscard]] bool matchesNext(const WorkResult& result) const;

    std::shared_ptr<DesktopBackend> backend_;
    EditController& editor_;
    JustifiedReviewLayoutModel& navigation_;
    QThreadPool pool_;
    QTimer idle_timer_;
    QFutureWatcher<WorkResult> watcher_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    std::shared_ptr<std::atomic<std::uint64_t>> render_token_;
    QVariantMap next_target_;
    QVariantMap previous_target_;
    QString origin_photo_id_;
    QString origin_representation_id_;
    bool enabled_ = false;
    bool at_loaded_end_ = false;
    bool work_outstanding_ = false;
    State state_ = State::Unavailable;
};

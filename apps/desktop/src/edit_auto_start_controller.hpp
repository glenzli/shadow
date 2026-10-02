#pragma once
#include "auto_start_analysis.hpp"
#include <QFutureWatcher>
#include <QObject>
#include <QSet>
#include <QTimer>
class EditController;
struct AutoStartPreviewResult final {
    BackendEditedPreview preview;
    QString error;
    std::uint64_t revision = 0;
};
struct AutoStartApplyResult final {
    BackendPhotoEditState state;
    QString error;
};

// Disposable proposal lifecycle. Analysis and candidate rendering never edit
// the Recipe; a single atomic apply joins the existing undo/redo history.
class EditAutoStartController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool analyzing READ analyzing NOTIFY changed)
    Q_PROPERTY(bool applying READ applying NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool canApply READ canApply NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString originalSource READ originalSource NOTIFY changed)
    Q_PROPERTY(QString previewSource READ previewSource NOTIFY changed)
    Q_PROPERTY(double strength READ strength WRITE setStrength NOTIFY changed)
    Q_PROPERTY(
        bool whiteBalanceEnabled READ whiteBalanceEnabled WRITE setWhiteBalanceEnabled NOTIFY
            changed
    )
    Q_PROPERTY(bool toneEnabled READ toneEnabled WRITE setToneEnabled NOTIFY changed)
    Q_PROPERTY(bool skinEnabled READ skinEnabled WRITE setSkinEnabled NOTIFY changed)
    Q_PROPERTY(bool whiteBalanceAvailable READ whiteBalanceAvailable NOTIFY changed)
    Q_PROPERTY(bool toneAvailable READ toneAvailable NOTIFY changed)
    Q_PROPERTY(bool skinAvailable READ skinAvailable NOTIFY changed)
    Q_PROPERTY(bool automaticEnabled READ automaticEnabled WRITE setAutomaticEnabled NOTIFY changed)
  public:
    EditAutoStartController(EditController& owner, std::shared_ptr<DesktopBackend> backend);
    ~EditAutoStartController() override;
    bool active() const {
        return active_;
    }
    bool analyzing() const {
        return pending_ || analysis_.isRunning();
    }
    bool busy() const {
        return analyzing() || preview_.isRunning() || preview_timer_.isActive() || applying_;
    }
    bool applying() const {
        return applying_;
    }
    bool ready() const {
        return active_ && !proposal_.original.isEmpty();
    }
    bool canApply() const;
    bool sceneAnalyzed() const {
        return proposal_.sceneChecked;
    }
    QString status() const {
        return status_;
    }
    QString summary() const;
    QString originalSource() const;
    QString previewSource() const {
        return preview_source_;
    }
    double strength() const {
        return strength_;
    }
    bool whiteBalanceEnabled() const {
        return white_balance_;
    }
    bool toneEnabled() const {
        return tone_;
    }
    bool skinEnabled() const {
        return skin_;
    }
    bool whiteBalanceAvailable() const {
        return proposal_.whiteBalance.available && !proposal_.tone.preserveLight;
    }
    bool toneAvailable() const {
        return proposal_.tone.useful;
    }
    bool skinAvailable() const {
        return !proposal_.skinNodes.isEmpty();
    }
    bool automaticEnabled() const;
    void setStrength(double);
    void setWhiteBalanceEnabled(bool);
    void setToneEnabled(bool);
    void setSkinEnabled(bool);
    void setAutomaticEnabled(bool);
    Q_INVOKABLE void analyze();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void apply();
  signals:
    void changed();
    void applied();

  private:
    bool current() const;
    void notify();
    void startPending();
    void finishAnalysis();
    void schedulePreview();
    void startPreview();
    void finishPreview();
    void finishApply();
    void maybeAutomatic();
    void discard(AutoStartProposal&);
    BackendGradeStack candidate() const;
    EditController& owner_;
    std::shared_ptr<DesktopBackend> backend_;
    QFutureWatcher<AutoStartProposal> analysis_;
    QFutureWatcher<AutoStartPreviewResult> preview_;
    QFutureWatcher<AutoStartApplyResult> apply_;
    QTimer preview_timer_, automatic_timer_;
    std::shared_ptr<AutoStartCancellation> cancellation_;
    BackendGradeStack before_;
    BackendGradeNode tone_node_;
    AutoStartProposal proposal_;
    QString photo_, source_, base_, variant_, status_, original_source_, preview_source_;
    QSet<QString> automatically_attempted_;
    std::uint64_t generation_ = 0, photo_generation_ = 0, preview_revision_ = 0,
                  presented_revision_ = 0, render_token_ = 0;
    double strength_ = 1;
    bool active_ = false, pending_ = false, applying_ = false, white_balance_ = true, tone_ = true,
         skin_ = true;
};

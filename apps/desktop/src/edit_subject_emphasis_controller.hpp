#pragma once

#include "desktop_backend.hpp"
#include <QFutureWatcher>
#include <QObject>
#include <QStringList>

class EditController;

struct SubjectEmphasisTaskResult final {
    BackendSubjectMaskResult result;
    QString error;
};
struct SubjectEmphasisApplyResult final {
    BackendPhotoEditState state;
    QString error;
};

// Owns the disposable analysis/selection draft. Only apply creates a Grade
// Node; cancellation leaves both the Recipe and undo history untouched.
class EditSubjectEmphasisController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool applying READ applying NOTIFY changed)
    Q_PROPERTY(bool analyzed READ analyzed NOTIFY changed)
    Q_PROPERTY(bool canApply READ canApply NOTIFY changed)
    Q_PROPERTY(bool applied READ applied NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString description READ description NOTIFY changed)
    Q_PROPERTY(QStringList queries READ queries NOTIFY changed)
    Q_PROPERTY(QString imageSource READ imageSource NOTIFY changed)
    Q_PROPERTY(QString maskSource READ maskSource NOTIFY changed)
    Q_PROPERTY(double strength READ strength WRITE setStrength NOTIFY changed)
  public:
    EditSubjectEmphasisController(EditController& owner, std::shared_ptr<DesktopBackend> backend);
    ~EditSubjectEmphasisController() override;
    bool active() const noexcept {
        return active_;
    }
    bool busy() const noexcept {
        return pending_ || worker_.isRunning() || applying_;
    }
    bool applying() const noexcept {
        return applying_;
    }
    bool analyzed() const noexcept {
        return !image_source_.isEmpty();
    }
    bool canApply() const noexcept;
    bool applied() const noexcept;
    QString status() const {
        return !active_ && !applied_id_.isEmpty() && !applied() ? QString{} : status_;
    }
    QString description() const {
        return description_;
    }
    QStringList queries() const {
        return queries_;
    }
    QString imageSource() const {
        return image_source_;
    }
    QString maskSource() const {
        return mask_source_;
    }
    double strength() const noexcept;
    void setStrength(double value);
    Q_INVOKABLE void analyze();
    Q_INVOKABLE void selectSubject(const QString& query);
    Q_INVOKABLE void apply();
    Q_INVOKABLE void cancel();
  signals:
    void changed();

  private:
    bool current() const noexcept;
    void notify();
    void startPending();
    void finish();
    void finishApply();
    void retireProposal();
    void retireInput();
    EditController& owner_;
    std::shared_ptr<DesktopBackend> backend_;
    QFutureWatcher<SubjectEmphasisTaskResult> worker_;
    QFutureWatcher<SubjectEmphasisApplyResult> apply_worker_;
    BackendGradeStack before_;
    BackendGradeStack draft_;
    BackendSubjectMaskResult candidate_;
    QString photo_id_, source_path_, variant_id_, target_id_, applied_id_, query_;
    QString status_, description_, image_source_, mask_source_;
    QStringList queries_;
    std::uint64_t photo_generation_ = 0, generation_ = 0, job_ = 0, input_ = 0;
    BackendSubjectMaskKind kind_ = BackendSubjectMaskKind::SubjectAnalysis;
    double strength_ = 1.0;
    bool active_ = false, pending_ = false, applying_ = false;
};

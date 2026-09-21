#pragma once
#include "backend/edit_types.hpp"
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <optional>
class EditController;
struct TargetedCurveInputResult final {
    BackendCurveInputMap map;
    QString error;
};

// Owns one immutable input-map request and one image-space curve gesture.
class EditTargetedCurveController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool dragging READ dragging NOTIFY changed)
    Q_PROPERTY(double inputValue READ inputValue NOTIFY sampleChanged)
    Q_PROPERTY(double outputValue READ outputValue NOTIFY sampleChanged)
    Q_PROPERTY(QString status READ status NOTIFY changed)
  public:
    explicit EditTargetedCurveController(EditController& owner);
    ~EditTargetedCurveController() override;
    bool active() const {
        return active_;
    }
    bool busy() const {
        return watcher_.isRunning() || debounce_.isActive();
    }
    bool ready() const {
        return active_ && !map_.values.isEmpty();
    }
    bool dragging() const {
        return before_.has_value();
    }
    double inputValue() const {
        return input_;
    }
    double outputValue() const {
        return output_;
    }
    QString status() const;
    void setActive(bool active);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void hover(double x, double y);
    Q_INVOKABLE bool begin(double x, double y);
    Q_INVOKABLE void move(double verticalDelta);
    Q_INVOKABLE void finish(bool cancel = false);
  signals:
    void activeChanged();
    void changed();
    void sampleChanged();

  private:
    void prepare();
    void accept();
    void reconcile();
    BackendGradeStack inputKey() const;
    EditController& owner_;
    QFutureWatcher<TargetedCurveInputResult> watcher_;
    QTimer debounce_;
    BackendCurveInputMap map_;
    QVariantList sampled_curve_;
    BackendGradeStack key_;
    std::optional<BackendGradeStack> before_;
    quint64 sequence_ = 0, requested_sequence_ = 0, token_ = 0, photo_generation_ = 0;
    int channel_ = 0, point_ = -1;
    double input_ = -1, output_ = 0, origin_output_ = 0;
    bool active_ = false, applying_ = false, failed_ = false;
};

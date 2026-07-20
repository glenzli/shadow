#pragma once

#include "desktop_backend.hpp"
#include "edit_history.hpp"
#include "edit_preview_contract.hpp"
#include "edit_preview_provider.hpp"
#include "edit_version_model.hpp"
#include "tone_curve_point_model.hpp"

#include <QAbstractItemModel>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <memory>

enum class EditStateTaskKind : std::uint8_t {
    Open,
    Save,
    Checkout,
};

struct EditStateTaskResult final {
    BackendPhotoEditState state;
    QString error;
    quint64 photo_generation = 0;
    EditStateTaskKind kind = EditStateTaskKind::Open;
};

struct EditPreviewTaskResult final {
    BackendEditedPreview preview;
    QString error;
    EditPreviewGeneration generation;
};

class EditController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool stateBusy READ stateBusy NOTIFY stateBusyChanged)
    Q_PROPERTY(bool rendering READ rendering NOTIFY renderingChanged)
    Q_PROPERTY(bool beforeRendering READ beforeRendering NOTIFY beforeRenderingChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY sourcePathChanged)
    Q_PROPERTY(QString previewSource READ previewSource NOTIFY previewSourceChanged)
    Q_PROPERTY(
        QString beforePreviewSource
        READ beforePreviewSource
        NOTIFY beforePreviewSourceChanged
    )
    Q_PROPERTY(QString beforeErrorText READ beforeErrorText NOTIFY beforeErrorTextChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(
        bool layerEnabled
        READ layerEnabled
        WRITE setLayerEnabled
        NOTIFY layerEnabledChanged
    )
    Q_PROPERTY(
        double exposureStops
        READ exposureStops
        WRITE setExposureStops
        NOTIFY parametersChanged
    )
    Q_PROPERTY(
        double contrastFactor
        READ contrastFactor
        WRITE setContrastFactor
        NOTIFY parametersChanged
    )
    Q_PROPERTY(double redGain READ redGain WRITE setRedGain NOTIFY parametersChanged)
    Q_PROPERTY(double greenGain READ greenGain WRITE setGreenGain NOTIFY parametersChanged)
    Q_PROPERTY(double blueGain READ blueGain WRITE setBlueGain NOTIFY parametersChanged)
    Q_PROPERTY(
        double saturationFactor
        READ saturationFactor
        WRITE setSaturationFactor
        NOTIFY parametersChanged
    )
    Q_PROPERTY(QAbstractItemModel* toneCurvePoints READ toneCurvePoints CONSTANT)
    Q_PROPERTY(bool hasToneCurve READ hasToneCurve NOTIFY toneCurveChanged)
    Q_PROPERTY(bool toneCurveEditable READ toneCurveEditable NOTIFY toneCurveChanged)
    Q_PROPERTY(QAbstractItemModel* versions READ versions CONSTANT)

public:
    explicit EditController(
        std::shared_ptr<DesktopBackend> backend,
        std::shared_ptr<EditPreviewStore> preview_store,
        QObject* parent = nullptr
    );
    ~EditController() override;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool stateBusy() const noexcept;
    [[nodiscard]] bool rendering() const noexcept;
    [[nodiscard]] bool beforeRendering() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] QString title() const;
    [[nodiscard]] QString sourcePath() const;
    [[nodiscard]] QString previewSource() const;
    [[nodiscard]] QString beforePreviewSource() const;
    [[nodiscard]] QString beforeErrorText() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] bool layerEnabled() const noexcept;
    [[nodiscard]] double exposureStops() const noexcept;
    [[nodiscard]] double contrastFactor() const noexcept;
    [[nodiscard]] double redGain() const noexcept;
    [[nodiscard]] double greenGain() const noexcept;
    [[nodiscard]] double blueGain() const noexcept;
    [[nodiscard]] double saturationFactor() const noexcept;
    [[nodiscard]] QAbstractItemModel* toneCurvePoints() noexcept;
    [[nodiscard]] bool hasToneCurve() const noexcept;
    [[nodiscard]] bool toneCurveEditable() const noexcept;
    [[nodiscard]] QAbstractItemModel* versions() noexcept;

    void setLayerEnabled(bool enabled);
    void setExposureStops(double value);
    void setContrastFactor(double value);
    void setRedGain(double value);
    void setGreenGain(double value);
    void setBlueGain(double value);
    void setSaturationFactor(double value);

    Q_INVOKABLE void openPhoto(
        const QString& photo_id,
        const QString& representation_id,
        const QString& source_path,
        const QString& title
    );
    Q_INVOKABLE void closePhoto();
    Q_INVOKABLE void beginParameterEdit(const QString& parameter_key);
    Q_INVOKABLE void endParameterEdit(const QString& parameter_key);
    Q_INVOKABLE void beginToneCurveGesture(int index);
    Q_INVOKABLE void moveToneCurvePoint(int index, double x, double y);
    Q_INVOKABLE void endToneCurveGesture(int index);
    Q_INVOKABLE void addToneCurvePoint(double x, double y);
    Q_INVOKABLE void removeToneCurvePoint(int index);
    Q_INVOKABLE void resetToneCurve();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void resetEdits();
    Q_INVOKABLE void revertEdits();
    Q_INVOKABLE void requestBeforePreview();
    Q_INVOKABLE void saveVersion(const QString& version_name);
    Q_INVOKABLE void checkoutVersion(const QString& commit_id);

signals:
    void activeChanged();
    void busyChanged();
    void stateBusyChanged();
    void renderingChanged();
    void beforeRenderingChanged();
    void dirtyChanged();
    void historyChanged();
    void titleChanged();
    void sourcePathChanged();
    void previewSourceChanged();
    void beforePreviewSourceChanged();
    void beforeErrorTextChanged();
    void statusTextChanged();
    void layerEnabledChanged();
    void parametersChanged();
    void toneCurveChanged();

private slots:
    void finishStateTask();
    void finishPreviewTask();
    void startPreviewRender();

private:
    void applyState(BackendPhotoEditState state);
    void setSettings(BackendEditSettings settings);
    void clearSessionHistory();
    void recordWorkingTransition(
        const QString& key,
        const BackendEditSettings& before
    );
    void schedulePreview(int delay_ms);
    void maybeStartBeforePreview();
    void setStatusText(QString status);
    void setDirty(bool dirty);
    void setStateRunning(bool running);
    void setPreviewRunning(EditPreviewKind kind, bool running);
    void emitBusyChange(bool previous_busy);
    void parameterEdited(
        const QString& key,
        const BackendEditSettings& before
    );
    void toneCurveEdited(
        const QString& key,
        const BackendEditSettings& before,
        int preview_delay_ms
    );
    [[nodiscard]] bool acceptParameter(
        double value,
        double minimum,
        double maximum,
        const QString& label
    );

    std::shared_ptr<DesktopBackend> backend_;
    std::shared_ptr<EditPreviewStore> preview_store_;
    EditVersionModel versions_;
    ToneCurvePointModel tone_curve_points_;
    QFutureWatcher<EditStateTaskResult> state_watcher_;
    QFutureWatcher<EditPreviewTaskResult> preview_watcher_;
    QTimer preview_debounce_;
    SessionEditHistory<BackendEditSettings> history_;
    BackendEditSettings settings_;
    BackendEditSettings committed_settings_;
    QString working_commit_id_;
    QString photo_id_;
    QString representation_id_;
    QString source_path_;
    QString title_;
    QString preview_source_;
    QString before_preview_source_;
    QString before_error_text_;
    QString status_text_ = QStringLiteral("Open a photo from Review to begin editing");
    quint64 photo_generation_ = 0;
    quint64 render_revision_ = 0;
    quint64 settled_render_revision_ = 0;
    bool active_ = false;
    bool dirty_ = false;
    bool state_running_ = false;
    bool current_rendering_ = false;
    bool before_rendering_ = false;
    bool preview_queued_ = false;
    bool before_requested_ = false;
};

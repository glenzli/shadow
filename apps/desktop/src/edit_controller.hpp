#pragma once

#include "desktop_backend.hpp"
#include "edit_history.hpp"
#include "edit_preview_contract.hpp"
#include "edit_preview_provider.hpp"
#include "edit_version_model.hpp"
#include "localized_ui_message.hpp"
#include "tone_curve_point_model.hpp"

#include <QAbstractItemModel>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <memory>

enum class EditStateTaskKind : std::uint8_t {
    Open,
    Save,
    LoadDraft,
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

struct EditDetailTaskResult final {
    BackendEditedDetailViewport viewport;
    QString error;
    EditDetailGeneration generation;
};

class EditController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool stateBusy READ stateBusy NOTIFY stateBusyChanged)
    Q_PROPERTY(bool rendering READ rendering NOTIFY renderingChanged)
    Q_PROPERTY(bool beforeRendering READ beforeRendering NOTIFY beforeRenderingChanged)
    Q_PROPERTY(bool detailMode READ detailMode NOTIFY detailModeChanged)
    Q_PROPERTY(bool detailRendering READ detailRendering NOTIFY detailRenderingChanged)
    Q_PROPERTY(QString detailErrorText READ detailErrorText NOTIFY detailErrorTextChanged)
    Q_PROPERTY(quint32 detailFullWidth READ detailFullWidth NOTIFY detailGeometryChanged)
    Q_PROPERTY(quint32 detailFullHeight READ detailFullHeight NOTIFY detailGeometryChanged)
    Q_PROPERTY(quint64 detailRetainedBytes READ detailRetainedBytes NOTIFY detailGeometryChanged)
    Q_PROPERTY(QVariantList detailTiles READ detailTiles NOTIFY detailTilesChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool versionDraft READ versionDraft NOTIFY versionDraftChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString photoId READ photoId NOTIFY sourceIdentityChanged)
    Q_PROPERTY(
        QString representationId
        READ representationId
        NOTIFY sourceIdentityChanged
    )
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY sourcePathChanged)
    Q_PROPERTY(QString previewSource READ previewSource NOTIFY previewSourceChanged)
    Q_PROPERTY(
        QString provisionalPreviewSource
        READ provisionalPreviewSource
        NOTIFY provisionalPreviewSourceChanged
    )
    Q_PROPERTY(
        QString beforePreviewSource
        READ beforePreviewSource
        NOTIFY beforePreviewSourceChanged
    )
    Q_PROPERTY(QVariantMap histogram READ histogram NOTIFY histogramChanged)
    Q_PROPERTY(
        QVariantMap beforeHistogram
        READ beforeHistogram
        NOTIFY beforeHistogramChanged
    )
    Q_PROPERTY(QString beforeErrorText READ beforeErrorText NOTIFY beforeErrorTextChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool opticsEnabled READ opticsEnabled WRITE setOpticsEnabled NOTIFY opticsChanged)
    Q_PROPERTY(bool opticsDistortionEnabled READ opticsDistortionEnabled WRITE setOpticsDistortionEnabled NOTIFY opticsChanged)
    Q_PROPERTY(bool opticsTcaEnabled READ opticsTcaEnabled WRITE setOpticsTcaEnabled NOTIFY opticsChanged)
    Q_PROPERTY(bool opticsVignettingEnabled READ opticsVignettingEnabled WRITE setOpticsVignettingEnabled NOTIFY opticsChanged)
    Q_PROPERTY(bool opticsAutomaticScale READ opticsAutomaticScale WRITE setOpticsAutomaticScale NOTIFY opticsChanged)
    Q_PROPERTY(QVariantMap opticsReceipt READ opticsReceipt NOTIFY opticsReceiptChanged)
    Q_PROPERTY(bool opticsManualProfile READ opticsManualProfile NOTIFY opticsChanged)
    Q_PROPERTY(QString opticsCameraProfile READ opticsCameraProfile NOTIFY opticsChanged)
    Q_PROPERTY(QString opticsLensProfile READ opticsLensProfile NOTIFY opticsChanged)
    Q_PROPERTY(QVariantList gradeNodes READ gradeNodes NOTIFY gradeNodesChanged)
    Q_PROPERTY(
        int selectedGradeNodeIndex
        READ selectedGradeNodeIndex
        NOTIFY selectedGradeNodeChanged
    )
    Q_PROPERTY(
        QString selectedGradeNodeId
        READ selectedGradeNodeId
        NOTIFY selectedGradeNodeChanged
    )
    Q_PROPERTY(
        bool hasSelectedGradeNode
        READ hasSelectedGradeNode
        NOTIFY selectedGradeNodeChanged
    )
    Q_PROPERTY(bool canAddGradeNode READ canAddGradeNode NOTIFY gradeNodeActionsChanged)
    Q_PROPERTY(
        bool canDeleteGradeNode
        READ canDeleteGradeNode
        NOTIFY gradeNodeActionsChanged
    )
    Q_PROPERTY(
        bool canMoveGradeNodeUp
        READ canMoveGradeNodeUp
        NOTIFY gradeNodeActionsChanged
    )
    Q_PROPERTY(
        bool canMoveGradeNodeDown
        READ canMoveGradeNodeDown
        NOTIFY gradeNodeActionsChanged
    )
    Q_PROPERTY(
        bool gradeNodeEnabled
        READ gradeNodeEnabled
        WRITE setGradeNodeEnabled
        NOTIFY gradeNodeEnabledChanged
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
    Q_PROPERTY(
        double whiteBalanceTemperature
        READ whiteBalanceTemperature
        WRITE setWhiteBalanceTemperature
        NOTIFY parametersChanged
    )
    Q_PROPERTY(
        double whiteBalanceTint
        READ whiteBalanceTint
        WRITE setWhiteBalanceTint
        NOTIFY parametersChanged
    )
    Q_PROPERTY(
        double saturationFactor
        READ saturationFactor
        WRITE setSaturationFactor
        NOTIFY parametersChanged
    )
    Q_PROPERTY(
        quint64 parameterRevision
        READ parameterRevision
        NOTIFY parametersChanged
    )
    Q_PROPERTY(QAbstractItemModel* toneCurvePoints READ toneCurvePoints CONSTANT)
    Q_PROPERTY(QVariantList pointColors READ pointColors NOTIFY parametersChanged)
    Q_PROPERTY(int selectedPointColorIndex READ selectedPointColorIndex NOTIFY parametersChanged)
    Q_PROPERTY(bool pointColorPickerActive READ pointColorPickerActive NOTIFY pointColorPickerActiveChanged)
    Q_PROPERTY(
        bool whiteBalancePickerActive
        READ whiteBalancePickerActive
        NOTIFY whiteBalancePickerActiveChanged
    )
    Q_PROPERTY(bool hasToneCurve READ hasToneCurve NOTIFY toneCurveChanged)
    Q_PROPERTY(bool hasAnyToneCurve READ hasAnyToneCurve NOTIFY toneCurveChanged)
    Q_PROPERTY(bool toneCurveSmooth READ toneCurveSmooth NOTIFY toneCurveChanged)
    Q_PROPERTY(
        int toneCurveChannel
        READ toneCurveChannel
        NOTIFY toneCurveChannelChanged
    )
    Q_PROPERTY(bool toneCurveEditable READ toneCurveEditable NOTIFY toneCurveChanged)
    Q_PROPERTY(QString lutResourceId READ lutResourceId NOTIFY parametersChanged)
    Q_PROPERTY(QString lutTitle READ lutTitle NOTIFY parametersChanged)
    Q_PROPERTY(bool hasLut READ hasLut NOTIFY parametersChanged)
    Q_PROPERTY(double lutIntensity READ lutIntensity WRITE setLutIntensity NOTIFY parametersChanged)
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
    [[nodiscard]] bool detailMode() const noexcept;
    [[nodiscard]] bool detailRendering() const noexcept;
    [[nodiscard]] QString detailErrorText() const;
    [[nodiscard]] quint32 detailFullWidth() const noexcept;
    [[nodiscard]] quint32 detailFullHeight() const noexcept;
    [[nodiscard]] quint64 detailRetainedBytes() const noexcept;
    [[nodiscard]] QVariantList detailTiles() const;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool versionDraft() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] QString photoId() const;
    [[nodiscard]] QString representationId() const;
    [[nodiscard]] QString title() const;
    [[nodiscard]] QString sourcePath() const;
    [[nodiscard]] QString previewSource() const;
    [[nodiscard]] QString provisionalPreviewSource() const;
    [[nodiscard]] QString beforePreviewSource() const;
    [[nodiscard]] QVariantMap histogram() const;
    [[nodiscard]] QVariantMap beforeHistogram() const;
    [[nodiscard]] QString beforeErrorText() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] bool opticsEnabled() const noexcept;
    [[nodiscard]] bool opticsDistortionEnabled() const noexcept;
    [[nodiscard]] bool opticsTcaEnabled() const noexcept;
    [[nodiscard]] bool opticsVignettingEnabled() const noexcept;
    [[nodiscard]] bool opticsAutomaticScale() const noexcept;
    [[nodiscard]] QVariantMap opticsReceipt() const;
    [[nodiscard]] bool opticsManualProfile() const noexcept;
    [[nodiscard]] QString opticsCameraProfile() const;
    [[nodiscard]] QString opticsLensProfile() const;
    [[nodiscard]] QVariantList gradeNodes() const;
    [[nodiscard]] int selectedGradeNodeIndex() const noexcept;
    [[nodiscard]] QString selectedGradeNodeId() const;
    [[nodiscard]] bool hasSelectedGradeNode() const noexcept;
    [[nodiscard]] bool canAddGradeNode() const noexcept;
    [[nodiscard]] bool canDeleteGradeNode() const noexcept;
    [[nodiscard]] bool canMoveGradeNodeUp() const noexcept;
    [[nodiscard]] bool canMoveGradeNodeDown() const noexcept;
    [[nodiscard]] bool gradeNodeEnabled() const noexcept;
    [[nodiscard]] double exposureStops() const noexcept;
    [[nodiscard]] double contrastFactor() const noexcept;
    [[nodiscard]] double whiteBalanceTemperature() const noexcept;
    [[nodiscard]] double whiteBalanceTint() const noexcept;
    [[nodiscard]] double saturationFactor() const noexcept;
    [[nodiscard]] quint64 parameterRevision() const noexcept;
    [[nodiscard]] QAbstractItemModel* toneCurvePoints() noexcept;
    [[nodiscard]] QVariantList pointColors() const;
    [[nodiscard]] int selectedPointColorIndex() const noexcept;
    [[nodiscard]] bool pointColorPickerActive() const noexcept;
    [[nodiscard]] bool whiteBalancePickerActive() const noexcept;
    [[nodiscard]] bool hasToneCurve() const noexcept;
    [[nodiscard]] bool hasAnyToneCurve() const noexcept;
    [[nodiscard]] bool toneCurveSmooth() const noexcept;
    [[nodiscard]] int toneCurveChannel() const noexcept;
    [[nodiscard]] bool toneCurveEditable() const noexcept;
    [[nodiscard]] QString lutResourceId() const;
    [[nodiscard]] QString lutTitle() const;
    [[nodiscard]] bool hasLut() const noexcept;
    [[nodiscard]] double lutIntensity() const noexcept;
    [[nodiscard]] QAbstractItemModel* versions() noexcept;

    void setGradeNodeEnabled(bool enabled);
    void setExposureStops(double value);
    void setContrastFactor(double value);
    void setWhiteBalanceTemperature(double value);
    void setWhiteBalanceTint(double value);
    void setSaturationFactor(double value);
    void setLutIntensity(double value);
    void setOpticsEnabled(bool enabled);
    void setOpticsDistortionEnabled(bool enabled);
    void setOpticsTcaEnabled(bool enabled);
    void setOpticsVignettingEnabled(bool enabled);
    void setOpticsAutomaticScale(bool enabled);

    Q_INVOKABLE bool openPhoto(
        const QString& photo_id,
        const QString& representation_id,
        const QString& source_path,
        const QString& title,
        const QString& provisional_preview_source = {}
    );
    Q_INVOKABLE void closePhoto();
    Q_INVOKABLE void selectGradeNode(int index);
    Q_INVOKABLE void addGradeNode();
    Q_INVOKABLE void duplicateSelectedGradeNode();
    Q_INVOKABLE void deleteSelectedGradeNode();
    Q_INVOKABLE void moveSelectedGradeNode(int destination_index);
    Q_INVOKABLE void beginParameterEdit(const QString& parameter_key);
    Q_INVOKABLE void endParameterEdit(const QString& parameter_key);
    Q_INVOKABLE double parameterValue(const QString& parameter_key) const;
    Q_INVOKABLE void setParameterValue(
        const QString& parameter_key,
        double value
    );
    Q_INVOKABLE void setColorGradingWheel(
        const QString& tonal_range,
        double hue,
        double saturation
    );
    Q_INVOKABLE void setDefringeHueRange(
        const QString& family,
        double lower_hue,
        double upper_hue
    );
    Q_INVOKABLE double colorMixerValue(int band_index, const QString& component) const;
    Q_INVOKABLE void setColorMixerValue(
        int band_index,
        const QString& component,
        double value
    );
    Q_INVOKABLE void setLutResource(
        const QString& resource_id,
        const QString& title,
        const QString& managed_path
    );
    Q_INVOKABLE void clearLut();
    Q_INVOKABLE QVariantList opticsProfileCandidates();
    Q_INVOKABLE void setManualOpticsProfile(
        const QString& camera_maker,
        const QString& camera_model,
        const QString& lens_maker,
        const QString& lens_model
    );
    Q_INVOKABLE void clearManualOpticsProfile();
    Q_INVOKABLE void selectPointColor(int index);
    Q_INVOKABLE void removeSelectedPointColor();
    Q_INVOKABLE void setPointColorPickerActive(bool active);
    Q_INVOKABLE void setWhiteBalancePickerActive(bool active);
    Q_INVOKABLE void setWhiteBalanceFromPreview(
        double normalized_x,
        double normalized_y,
        const QString& preview_generation
    );
    Q_INVOKABLE void addPointColorFromPreview(
        double normalized_x,
        double normalized_y,
        const QString& preview_generation
    );
    Q_INVOKABLE void selectToneCurveChannel(int channel);
    Q_INVOKABLE bool toneCurveChannelActive(int channel) const noexcept;
    Q_INVOKABLE void beginToneCurveGesture(int index);
    Q_INVOKABLE void moveToneCurvePoint(int index, double x, double y);
    Q_INVOKABLE void endToneCurveGesture(int index);
    Q_INVOKABLE void addToneCurvePoint(double x, double y);
    Q_INVOKABLE void removeToneCurvePoint(int index);
    Q_INVOKABLE void resetToneCurve();
    Q_INVOKABLE void resetAllToneCurves();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void resetSelectedGradeNode();
    Q_INVOKABLE void revertEdits();
    Q_INVOKABLE void requestBeforePreview();
    Q_INVOKABLE void requestDetailViewport(
        double center_x,
        double center_y,
        int viewport_width_pixels,
        int viewport_height_pixels
    );
    Q_INVOKABLE void leaveDetailMode();
    Q_INVOKABLE void saveVersion(const QString& version_name);
    Q_INVOKABLE void loadVersionDraft(const QString& commit_id);
  Q_INVOKABLE void retranslateUi();

signals:
    void activeChanged();
    void busyChanged();
    void stateBusyChanged();
    void renderingChanged();
    void beforeRenderingChanged();
    void detailModeChanged();
    void detailRenderingChanged();
    void detailErrorTextChanged();
    void detailGeometryChanged();
    void detailTilesChanged();
    void dirtyChanged();
    void versionDraftChanged();
    void historyChanged();
    void sourceIdentityChanged();
    void titleChanged();
    void sourcePathChanged();
    void previewSourceChanged();
    void provisionalPreviewSourceChanged();
    void beforePreviewSourceChanged();
    void histogramChanged();
    void beforeHistogramChanged();
    void beforeErrorTextChanged();
    void statusTextChanged();
    void opticsChanged();
    void opticsReceiptChanged();
    void gradeNodesChanged();
    void selectedGradeNodeChanged();
    void gradeNodeActionsChanged();
    void gradeNodeEnabledChanged();
    void parametersChanged();
    void toneCurveChanged();
    void toneCurveChannelChanged();
    void pointColorPickerActiveChanged();
    void whiteBalancePickerActiveChanged();

private slots:
    void finishStateTask();
    void finishPreviewTask();
    void finishDetailTask();
    void startPreviewRender();
    void startDetailRender();

private:
    void applyState(BackendPhotoEditState state);
    void setGradeStack(
        BackendGradeStack grade_stack,
        const QString& preferred_grade_node_id = {}
    );
    [[nodiscard]] const BackendGradeNode* selectedGradeNode() const noexcept;
    [[nodiscard]] QString gradeNodeHistoryKey(const QString& key) const;
    [[nodiscard]] QString uniqueGradeNodeLabel(const QString& base) const;
    void finishActiveGesture();
    void clearSessionHistory();
    void recordWorkingTransition(
        const QString& key,
        const BackendGradeStack& before
    );
    void schedulePreview(int delay_ms);
    void maybeStartBeforePreview();
    void maybeStartDetailRender();
    void invalidateDetailPresentation();
    void resetDetailState();
  bool eventFilter(QObject *watched, QEvent *event) override;
  void setStatusMessage(LocalizedUiMessage status);
    void setDirty(bool dirty);
    void setVersionDraft(bool draft);
    void setStateRunning(bool running);
    void setPreviewRunning(EditPreviewKind kind, bool running);
    void markHistogramUpdating(EditPreviewKind kind);
    void publishHistogram(
        EditPreviewKind kind,
        const BackendEditPreviewAnalysis& analysis,
        quint64 generation
    );
    void markHistogramFailed(EditPreviewKind kind);
    void clearHistograms();
    void setDetailRunning(bool running);
    void emitBusyChange(bool previous_busy);
    void parameterEdited(
        const QString& key,
        const BackendGradeStack& before
    );
    void opticsEdited(const QString& key, const BackendGradeStack& before);
    void notifyParametersChanged();
    void toneCurveEdited(
        const QString& key,
        const BackendGradeStack& before,
        int preview_delay_ms
    );
    [[nodiscard]] bool acceptParameter(
        double value,
        double minimum,
        double maximum,
        const char *label_source);

    std::shared_ptr<DesktopBackend> backend_;
    std::shared_ptr<EditPreviewStore> preview_store_;
    EditVersionModel versions_;
    ToneCurvePointModel tone_curve_points_;
    QFutureWatcher<EditStateTaskResult> state_watcher_;
    QFutureWatcher<EditPreviewTaskResult> preview_watcher_;
    QFutureWatcher<EditDetailTaskResult> detail_watcher_;
    QTimer preview_debounce_;
    QTimer detail_debounce_;
    SessionEditHistory<BackendGradeStack> history_;
    BackendGradeStack grade_stack_;
    BackendGradeStack committed_grade_stack_;
    QString base_commit_id_;
    QString durable_working_commit_id_;
    QString photo_id_;
    QString representation_id_;
    QString source_path_;
    QString title_;
    QString preview_source_;
    QString provisional_preview_source_;
    QString before_preview_source_;
    QVariantMap histogram_;
    QVariantMap before_histogram_;
    QVariantMap optics_receipt_;
  LocalizedUiMessage before_error_message_;
  LocalizedUiMessage detail_error_message_;
  LocalizedUiMessage status_message_{
      "EditController",
      QT_TRANSLATE_NOOP("EditController",
                        "Open a photo from Review to begin editing"),
  };
    quint64 photo_generation_ = 0;
    quint64 render_revision_ = 0;
    quint64 settled_render_revision_ = 0;
    quint64 detail_viewport_revision_ = 0;
    quint64 detail_render_token_ = 0;
    quint32 detail_full_width_ = 0;
    quint32 detail_full_height_ = 0;
    quint64 detail_retained_bytes_ = 0;
    QVariantList detail_tiles_;
    double detail_center_x_ = 0.5;
    double detail_center_y_ = 0.5;
    std::uint32_t detail_viewport_width_ = 1;
    std::uint32_t detail_viewport_height_ = 1;
    bool active_ = false;
    bool dirty_ = false;
    bool version_draft_ = false;
    bool state_running_ = false;
    bool current_rendering_ = false;
    bool before_rendering_ = false;
    bool detail_mode_ = false;
    bool detail_rendering_ = false;
    bool preview_queued_ = false;
    bool before_requested_ = false;
    bool detail_queued_ = false;
    int selected_grade_node_index_ = -1;
    int tone_curve_channel_ = 0;
    int selected_point_color_index_ = -1;
    bool point_color_picker_active_ = false;
    bool white_balance_picker_active_ = false;
    quint64 parameter_revision_ = 0;
};

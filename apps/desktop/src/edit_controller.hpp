#pragma once

#include "desktop_backend.hpp"
#include "edit_history.hpp"
#include "edit_persistence_state.hpp"
#include "edit_preview_contract.hpp"
#include "edit_preview_provider.hpp"
#include "edit_task_runner.hpp"
#include "edit_version_model.hpp"
#include "localized_ui_message.hpp"
#include "preview_diagnostics.hpp"
#include "tone_curve_point_model.hpp"

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QFuture>
#include <QFutureWatcher>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <memory>
#include <optional>

class EditPreviewPresentationContext;
class EditAiCompletionController;
class EditAiMaskController;
class EditAutoGeometryController;
class EditPersistenceTaskCoordinator;
class EditRawFoundationController;
class AiPreferences;

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
    Q_PROPERTY(quint32 levelZeroWidth READ levelZeroWidth NOTIFY previewGeometryChanged)
    Q_PROPERTY(quint32 levelZeroHeight READ levelZeroHeight NOTIFY previewGeometryChanged)
    Q_PROPERTY(quint64 detailRetainedBytes READ detailRetainedBytes NOTIFY detailGeometryChanged)
    Q_PROPERTY(QVariantList detailTiles READ detailTiles NOTIFY detailTilesChanged)
    Q_PROPERTY(
        bool fullResolutionPreparing READ fullResolutionPreparing NOTIFY fullResolutionStateChanged
    )
    Q_PROPERTY(bool fullResolutionReady READ fullResolutionReady NOTIFY fullResolutionStateChanged)
    Q_PROPERTY(
        quint64 fullResolutionRetainedBytes READ fullResolutionRetainedBytes NOTIFY
            fullResolutionStateChanged
    )
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(bool autosavePending READ autosavePending NOTIFY autosavePendingChanged)
    Q_PROPERTY(bool autosaveFailed READ autosaveFailed NOTIFY autosaveFailedChanged)
    Q_PROPERTY(QString autosaveErrorText READ autosaveErrorText NOTIFY autosaveErrorTextChanged)
    Q_PROPERTY(bool versionDraft READ versionDraft NOTIFY versionDraftChanged)
    Q_PROPERTY(QString editBaseCommitId READ editBaseCommitId NOTIFY editBaseCommitIdChanged)
    Q_PROPERTY(QString activeVariantId READ activeVariantId NOTIFY photoVariantsChanged)
    Q_PROPERTY(QVariantList photoVariants READ photoVariants NOTIFY photoVariantsChanged)
    Q_PROPERTY(bool variantActionsEnabled READ variantActionsEnabled NOTIFY variantActionsChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QString photoId READ photoId NOTIFY sourceIdentityChanged)
    Q_PROPERTY(QString representationId READ representationId NOTIFY sourceIdentityChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY sourcePathChanged)
    Q_PROPERTY(QString previewSource READ previewSource NOTIFY previewSourceChanged)
    Q_PROPERTY(
        QString provisionalPreviewSource READ provisionalPreviewSource NOTIFY
            provisionalPreviewSourceChanged
    )
    Q_PROPERTY(
        QString beforePreviewSource READ beforePreviewSource NOTIFY beforePreviewSourceChanged
    )
    Q_PROPERTY(QVariantMap histogram READ histogram NOTIFY histogramChanged)
    Q_PROPERTY(QVariantMap beforeHistogram READ beforeHistogram NOTIFY beforeHistogramChanged)
    Q_PROPERTY(QString beforeErrorText READ beforeErrorText NOTIFY beforeErrorTextChanged)
    Q_PROPERTY(bool recipeRecoveryRequired READ recipeRecoveryRequired NOTIFY recipeRecoveryChanged)
    Q_PROPERTY(
        QString recipeRecoveryErrorText READ recipeRecoveryErrorText NOTIFY recipeRecoveryChanged
    )
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    // Absolute photographer-facing source interpretation. These values belong
    // to the one photo Foundation and remain stable when another Grade Node is
    // selected, disabled, duplicated, reordered, or shared.
    Q_PROPERTY(
        bool foundationEnabled READ foundationEnabled WRITE setFoundationEnabled NOTIFY
            foundationChanged
    )
    Q_PROPERTY(
        int foundationWhiteBalanceTemperature READ foundationWhiteBalanceTemperature WRITE
            setFoundationWhiteBalanceTemperature NOTIFY foundationChanged
    )
    Q_PROPERTY(
        int foundationWhiteBalanceTint READ foundationWhiteBalanceTint WRITE
            setFoundationWhiteBalanceTint NOTIFY foundationChanged
    )
    Q_PROPERTY(
        bool foundationWhiteBalanceAtCameraValue READ foundationWhiteBalanceAtCameraValue NOTIFY
            foundationChanged
    )
    Q_PROPERTY(
        bool foundationWhiteBalanceCameraValueAvailable READ
            foundationWhiteBalanceCameraValueAvailable NOTIFY foundationChanged
    )
    Q_PROPERTY(
        bool rawDenoiseNodeMaterialized READ rawDenoiseNodeMaterialized NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        bool rawDenoiseNodeVisible READ rawDenoiseNodeVisible WRITE setRawDenoiseNodeVisible NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        bool foundationAiDenoiseEnabled READ foundationAiDenoiseEnabled WRITE
            setFoundationAiDenoiseEnabled NOTIFY foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        bool foundationAiDenoiseRequested READ foundationAiDenoiseRequested NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        int foundationAiDenoiseAmount READ foundationAiDenoiseAmount WRITE
            setFoundationAiDenoiseAmount NOTIFY foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        bool foundationAiDenoiseAvailable READ foundationAiDenoiseAvailable NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        bool foundationAiDenoiseBusy READ foundationAiDenoiseBusy NOTIFY foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        bool foundationAiDenoiseCanStart READ foundationAiDenoiseCanStart NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        bool foundationAiDenoiseCanApply READ foundationAiDenoiseCanApply NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        bool foundationAiDenoiseCanCancel READ foundationAiDenoiseCanCancel NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        QString foundationAiDenoisePhase READ foundationAiDenoisePhase NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        double foundationAiDenoiseProgress READ foundationAiDenoiseProgress NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        QString foundationAiDenoiseStatusText READ foundationAiDenoiseStatusText NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        bool foundationAiDenoiseNoiseAssessmentBusy READ foundationAiDenoiseNoiseAssessmentBusy
            NOTIFY foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        QString foundationAiDenoiseNoiseLevel READ foundationAiDenoiseNoiseLevel NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        int foundationAiDenoiseNoiseScore READ foundationAiDenoiseNoiseScore NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        int foundationAiDenoiseNoiseConfidence READ foundationAiDenoiseNoiseConfidence NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        QString foundationAiDenoiseNoiseRecommendation READ foundationAiDenoiseNoiseRecommendation
            NOTIFY foundationAiDenoiseChanged
    )
    Q_PROPERTY(bool opticsEnabled READ opticsEnabled WRITE setOpticsEnabled NOTIFY opticsChanged)
    Q_PROPERTY(
        bool opticsDistortionEnabled READ opticsDistortionEnabled WRITE setOpticsDistortionEnabled
            NOTIFY opticsChanged
    )
    Q_PROPERTY(
        bool opticsTcaEnabled READ opticsTcaEnabled WRITE setOpticsTcaEnabled NOTIFY opticsChanged
    )
    Q_PROPERTY(
        bool opticsVignettingEnabled READ opticsVignettingEnabled WRITE setOpticsVignettingEnabled
            NOTIFY opticsChanged
    )
    Q_PROPERTY(
        bool opticsAutomaticScale READ opticsAutomaticScale WRITE setOpticsAutomaticScale NOTIFY
            opticsChanged
    )
    Q_PROPERTY(
        int manualOpticsDistortion READ manualOpticsDistortion WRITE setManualOpticsDistortion
            NOTIFY opticsChanged
    )
    Q_PROPERTY(
        int manualOpticsTcaRedCyan READ manualOpticsTcaRedCyan WRITE setManualOpticsTcaRedCyan
            NOTIFY opticsChanged
    )
    Q_PROPERTY(
        int manualOpticsTcaBlueYellow READ manualOpticsTcaBlueYellow WRITE
            setManualOpticsTcaBlueYellow NOTIFY opticsChanged
    )
    Q_PROPERTY(
        int manualOpticsVignettingAmount READ manualOpticsVignettingAmount WRITE
            setManualOpticsVignettingAmount NOTIFY opticsChanged
    )
    Q_PROPERTY(
        int manualOpticsVignettingMidpoint READ manualOpticsVignettingMidpoint WRITE
            setManualOpticsVignettingMidpoint NOTIFY opticsChanged
    )
    Q_PROPERTY(QVariantMap opticsReceipt READ opticsReceipt NOTIFY opticsReceiptChanged)
    Q_PROPERTY(bool opticsManualProfile READ opticsManualProfile NOTIFY opticsChanged)
    Q_PROPERTY(QString opticsCameraProfile READ opticsCameraProfile NOTIFY opticsChanged)
    Q_PROPERTY(QString opticsLensProfile READ opticsLensProfile NOTIFY opticsChanged)
    // A local mask belongs to the selected Grade Node instance, never to the
    // shareable adjustment graph. One node owns one ordered component vector;
    // QML edits the selected leaf while the renderer remains authoritative for
    // the final Base/Add/Subtract/Intersect coverage.
    Q_PROPERTY(QVariantList localMaskComponents READ localMaskComponents NOTIFY parametersChanged)
    Q_PROPERTY(
        int selectedLocalMaskComponentIndex READ selectedLocalMaskComponentIndex NOTIFY
            parametersChanged
    )
    Q_PROPERTY(QVariantMap selectedLocalMask READ selectedLocalMask NOTIFY parametersChanged)
    Q_PROPERTY(
        bool maskToolActive READ maskToolActive WRITE setMaskToolActive NOTIFY maskToolActiveChanged
    )
    Q_PROPERTY(QString maskCoverageSource READ maskCoverageSource NOTIFY maskCoverageSourceChanged)
    Q_PROPERTY(
        bool maskCoverageShowsSelectedComponent READ maskCoverageShowsSelectedComponent WRITE
            setMaskCoverageShowsSelectedComponent NOTIFY maskCoverageModeChanged
    )
    Q_PROPERTY(bool aiMaskPromptActive READ aiMaskPromptActive NOTIFY aiMaskPromptChanged)
    Q_PROPERTY(bool aiMaskBusy READ aiMaskBusy NOTIFY aiMaskPromptChanged)
    Q_PROPERTY(bool aiMaskFaceRegionMode READ aiMaskFaceRegionMode NOTIFY aiMaskPromptChanged)
    Q_PROPERTY(bool aiMaskSemanticMode READ aiMaskSemanticMode NOTIFY aiMaskPromptChanged)
    Q_PROPERTY(QString aiMaskSemanticQuery READ aiMaskSemanticQuery NOTIFY aiMaskPromptChanged)
    Q_PROPERTY(
        int aiMaskFaceRegion READ aiMaskFaceRegion WRITE setAiMaskFaceRegion NOTIFY
            aiMaskPromptChanged
    )
    Q_PROPERTY(QVariantList aiMaskPeople READ aiMaskPeople NOTIFY aiMaskPromptChanged)
    Q_PROPERTY(
        int aiMaskSelectedPerson READ aiMaskSelectedPerson WRITE setAiMaskSelectedPerson NOTIFY
            aiMaskPromptChanged
    )
    Q_PROPERTY(int aiMaskFaceRegionMask READ aiMaskFaceRegionMask NOTIFY aiMaskPromptChanged)
    Q_PROPERTY(
        bool aiMaskForegroundMode READ aiMaskForegroundMode WRITE setAiMaskForegroundMode NOTIFY
            aiMaskPromptChanged
    )
    Q_PROPERTY(QVariantList aiMaskPromptPoints READ aiMaskPromptPoints NOTIFY aiMaskPromptChanged)
    Q_PROPERTY(bool aiMaskCanGenerate READ aiMaskCanGenerate NOTIFY aiMaskPromptChanged)
    Q_PROPERTY(bool aiMaskHasCandidate READ aiMaskHasCandidate NOTIFY aiMaskPromptChanged)
    Q_PROPERTY(QString aiMaskCandidateSource READ aiMaskCandidateSource NOTIFY aiMaskPromptChanged)
    Q_PROPERTY(bool imageCompletionActive READ imageCompletionActive NOTIFY imageCompletionChanged)
    Q_PROPERTY(bool imageCompletionBusy READ imageCompletionBusy NOTIFY imageCompletionChanged)
    Q_PROPERTY(
        bool imageCompletionCanGenerate READ imageCompletionCanGenerate NOTIFY
            imageCompletionChanged
    )
    Q_PROPERTY(
        bool imageCompletionExecutionAllowed READ imageCompletionExecutionAllowed WRITE
            setImageCompletionExecutionAllowed NOTIFY imageCompletionChanged
    )
    Q_PROPERTY(
        bool imageCompletionHasCandidate READ imageCompletionHasCandidate NOTIFY
            imageCompletionChanged
    )
    Q_PROPERTY(
        QString imageCompletionCandidateSource READ imageCompletionCandidateSource NOTIFY
            imageCompletionChanged
    )
    Q_PROPERTY(
        QVariantList imageCompletionBrushPoints READ imageCompletionBrushPoints NOTIFY
            imageCompletionChanged
    )
    Q_PROPERTY(
        double imageCompletionBrushRadius READ imageCompletionBrushRadius WRITE
            setImageCompletionBrushRadius NOTIFY imageCompletionChanged
    )
    Q_PROPERTY(
        bool imageCompletionEraseMode READ imageCompletionEraseMode WRITE
            setImageCompletionEraseMode NOTIFY imageCompletionChanged
    )
    Q_PROPERTY(
        QVariantList imageCompletionRegions READ imageCompletionRegions NOTIFY parametersChanged
    )
    Q_PROPERTY(
        bool imageCompletionNodeMaterialized READ imageCompletionNodeMaterialized NOTIFY
            parametersChanged
    )
    Q_PROPERTY(
        bool imageCompletionNodeEnabled READ imageCompletionNodeEnabled WRITE
            setImageCompletionNodeEnabled NOTIFY parametersChanged
    )
    Q_PROPERTY(
        bool rawDenoiseExecutionAllowed READ rawDenoiseExecutionAllowed NOTIFY
            foundationAiDenoiseChanged
    )
    Q_PROPERTY(
        bool subjectMaskExecutionAllowed READ subjectMaskExecutionAllowed NOTIFY aiMaskPromptChanged
    )
    // This is an in-session geometry clipboard, not a Recipe asset. A paste
    // creates the selected node's own one-mask attachment on the current photo.
    Q_PROPERTY(bool hasCopiedNodeMask READ hasCopiedNodeMask NOTIFY nodeMaskClipboardChanged)
    // Retouch belongs to the whole photo, after every Grade Node. Unlike a
    // local mask it must remain usable even when the selected node is shared
    // or disabled.
    Q_PROPERTY(QVariantList retouchSpots READ retouchSpots NOTIFY parametersChanged)
    Q_PROPERTY(QVariantList retouchStrokes READ retouchStrokes NOTIFY parametersChanged)
    Q_PROPERTY(
        bool retouchNodeEnabled READ retouchNodeEnabled WRITE setRetouchNodeEnabled NOTIFY
            parametersChanged
    )
    // Liquify is one optional photo-private structural node. The controller
    // exposes its ordered gestures, not a reusable node-list identity.
    Q_PROPERTY(QVariantList liquifyStrokes READ liquifyStrokes NOTIFY parametersChanged)
    Q_PROPERTY(
        bool liquifyNodeEnabled READ liquifyNodeEnabled WRITE setLiquifyNodeEnabled NOTIFY
            parametersChanged
    )
    Q_PROPERTY(
        double liquifyBrushRadius READ liquifyBrushRadius WRITE setLiquifyBrushRadius NOTIFY
            liquifyBrushChanged
    )
    Q_PROPERTY(
        double liquifyBrushStrength READ liquifyBrushStrength WRITE setLiquifyBrushStrength NOTIFY
            liquifyBrushChanged
    )
    Q_PROPERTY(
        double liquifyBrushHardness READ liquifyBrushHardness WRITE setLiquifyBrushHardness NOTIFY
            liquifyBrushChanged
    )
    Q_PROPERTY(
        int liquifyBrushMode READ liquifyBrushMode WRITE setLiquifyBrushMode NOTIFY
            liquifyBrushChanged
    )
    Q_PROPERTY(bool liquifyCanReconstruct READ liquifyCanReconstruct NOTIFY parametersChanged)
    // Crop/orientation is photo-local too. It is intentionally not a Grade
    // Node control, because framing must never become a shared style.
    Q_PROPERTY(QVariantMap photoGeometry READ photoGeometry NOTIFY parametersChanged)
    Q_PROPERTY(bool autoGeometryBusy READ autoGeometryBusy NOTIFY autoGeometryChanged)
    Q_PROPERTY(bool autoGeometryCanAnalyze READ autoGeometryCanAnalyze NOTIFY autoGeometryChanged)
    Q_PROPERTY(bool autoGeometryHasProposal READ autoGeometryHasProposal NOTIFY autoGeometryChanged)
    Q_PROPERTY(bool autoGeometryPreviewing READ autoGeometryPreviewing NOTIFY autoGeometryChanged)
    Q_PROPERTY(int autoGeometryConfidence READ autoGeometryConfidence NOTIFY autoGeometryChanged)
    Q_PROPERTY(
        double autoGeometrySuggestedStraighten READ autoGeometrySuggestedStraighten NOTIFY
            autoGeometryChanged
    )
    Q_PROPERTY(
        double autoGeometrySuggestedVertical READ autoGeometrySuggestedVertical NOTIFY
            autoGeometryChanged
    )
    Q_PROPERTY(
        double autoGeometrySuggestedHorizontal READ autoGeometrySuggestedHorizontal NOTIFY
            autoGeometryChanged
    )
    Q_PROPERTY(
        int autoGeometrySupportingLines READ autoGeometrySupportingLines NOTIFY autoGeometryChanged
    )
    Q_PROPERTY(
        QString autoGeometryStatusText READ autoGeometryStatusText NOTIFY autoGeometryChanged
    )
    Q_PROPERTY(bool canvasNodeMaterialized READ canvasNodeMaterialized NOTIFY parametersChanged)
    Q_PROPERTY(
        bool canvasNodeEnabled READ canvasNodeEnabled WRITE setCanvasNodeEnabled NOTIFY
            parametersChanged
    )
    // Crop is edited against the complete oriented source rather than the
    // already-cropped output. This is transient presentation state only; the
    // persisted v1 Recipe remains the single owner of the actual bounds.
    Q_PROPERTY(
        bool cropToolActive READ cropToolActive WRITE setCropToolActive NOTIFY cropToolActiveChanged
    )
    Q_PROPERTY(QVariantList gradeNodes READ gradeNodes NOTIFY gradeNodesChanged)
    Q_PROPERTY(QVariantList sharedGradeNodes READ sharedGradeNodes NOTIFY sharedGradeNodesChanged)
    Q_PROPERTY(
        int selectedGradeNodeIndex READ selectedGradeNodeIndex NOTIFY selectedGradeNodeChanged
    )
    Q_PROPERTY(QString selectedGradeNodeId READ selectedGradeNodeId NOTIFY selectedGradeNodeChanged)
    Q_PROPERTY(bool hasSelectedGradeNode READ hasSelectedGradeNode NOTIFY selectedGradeNodeChanged)
    Q_PROPERTY(bool foundationSelected READ foundationSelected NOTIFY selectedGradeNodeChanged)
    Q_PROPERTY(bool rawDenoiseSelected READ rawDenoiseSelected NOTIFY selectedGradeNodeChanged)
    Q_PROPERTY(
        QString selectedRecipeNodeKind READ selectedRecipeNodeKind NOTIFY selectedGradeNodeChanged
    )
    Q_PROPERTY(bool retouchNodeMaterialized READ retouchNodeMaterialized NOTIFY parametersChanged)
    Q_PROPERTY(bool liquifyNodeMaterialized READ liquifyNodeMaterialized NOTIFY parametersChanged)
    Q_PROPERTY(bool canAddGradeNode READ canAddGradeNode NOTIFY gradeNodeActionsChanged)
    Q_PROPERTY(bool canDeleteGradeNode READ canDeleteGradeNode NOTIFY gradeNodeActionsChanged)
    Q_PROPERTY(bool canMoveGradeNodeUp READ canMoveGradeNodeUp NOTIFY gradeNodeActionsChanged)
    Q_PROPERTY(bool canMoveGradeNodeDown READ canMoveGradeNodeDown NOTIFY gradeNodeActionsChanged)
    Q_PROPERTY(
        bool gradeNodeEnabled READ gradeNodeEnabled WRITE setGradeNodeEnabled NOTIFY
            gradeNodeEnabledChanged
    )
    Q_PROPERTY(
        double gradeNodeStrength READ gradeNodeStrength WRITE setGradeNodeStrength NOTIFY
            parametersChanged
    )
    Q_PROPERTY(
        double exposureStops READ exposureStops WRITE setExposureStops NOTIFY parametersChanged
    )
    Q_PROPERTY(
        double contrastFactor READ contrastFactor WRITE setContrastFactor NOTIFY parametersChanged
    )
    Q_PROPERTY(
        double whiteBalanceTemperature READ whiteBalanceTemperature WRITE setWhiteBalanceTemperature
            NOTIFY parametersChanged
    )
    Q_PROPERTY(
        double whiteBalanceTint READ whiteBalanceTint WRITE setWhiteBalanceTint NOTIFY
            parametersChanged
    )
    Q_PROPERTY(
        double saturationFactor READ saturationFactor WRITE setSaturationFactor NOTIFY
            parametersChanged
    )
    Q_PROPERTY(quint64 parameterRevision READ parameterRevision NOTIFY parametersChanged)
    Q_PROPERTY(QAbstractItemModel* toneCurvePoints READ toneCurvePoints CONSTANT)
    Q_PROPERTY(QVariantList pointColors READ pointColors NOTIFY parametersChanged)
    Q_PROPERTY(
        QVariantList colorWarperControlPoints READ colorWarperControlPoints NOTIFY parametersChanged
    )
    Q_PROPERTY(int selectedPointColorIndex READ selectedPointColorIndex NOTIFY parametersChanged)
    Q_PROPERTY(
        bool pointColorScopeActive READ pointColorScopeActive WRITE setPointColorScopeActive NOTIFY
            pointColorScopeChanged
    )
    Q_PROPERTY(bool pointColorScopeAvailable READ pointColorScopeAvailable NOTIFY parametersChanged)
    Q_PROPERTY(
        bool pointColorPickerActive READ pointColorPickerActive NOTIFY pointColorPickerActiveChanged
    )
    Q_PROPERTY(bool retouchPickerActive READ retouchPickerActive NOTIFY retouchPickerActiveChanged)
    Q_PROPERTY(
        int retouchCreationMode READ retouchCreationMode WRITE setRetouchCreationMode NOTIFY
            retouchCreationModeChanged
    )
    Q_PROPERTY(
        int retouchBrushRadius READ retouchBrushRadius WRITE setRetouchBrushRadius NOTIFY
            retouchBrushChanged
    )
    Q_PROPERTY(
        double retouchBrushFeather READ retouchBrushFeather WRITE setRetouchBrushFeather NOTIFY
            retouchBrushChanged
    )
    Q_PROPERTY(
        double retouchBrushStrength READ retouchBrushStrength WRITE setRetouchBrushStrength NOTIFY
            retouchBrushChanged
    )
    Q_PROPERTY(
        bool retouchSourceAligned READ retouchSourceAligned WRITE setRetouchSourceAligned NOTIFY
            retouchSourceChanged
    )
    Q_PROPERTY(bool retouchSourcePicking READ retouchSourcePicking NOTIFY retouchSourceChanged)
    Q_PROPERTY(bool retouchSourceSampled READ retouchSourceSampled NOTIFY retouchSourceChanged)
    Q_PROPERTY(
        QVariantMap retouchSampledSource READ retouchSampledSource NOTIFY retouchSourceChanged
    )
    Q_PROPERTY(
        bool whiteBalancePickerActive READ whiteBalancePickerActive NOTIFY
            whiteBalancePickerActiveChanged
    )
    Q_PROPERTY(
        bool rawWhiteBalancePickerActive READ rawWhiteBalancePickerActive NOTIFY
            rawWhiteBalancePickerActiveChanged
    )
    Q_PROPERTY(bool hasToneCurve READ hasToneCurve NOTIFY toneCurveChanged)
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
        std::shared_ptr<EditPreviewPresentationContext> preview_presentation_context,
        AiPreferences* ai_preferences = nullptr,
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
    [[nodiscard]] quint32 levelZeroWidth() const noexcept;
    [[nodiscard]] quint32 levelZeroHeight() const noexcept;
    [[nodiscard]] quint64 detailRetainedBytes() const noexcept;
    [[nodiscard]] QVariantList detailTiles() const;
    [[nodiscard]] bool fullResolutionPreparing() const noexcept;
    [[nodiscard]] bool fullResolutionReady() const noexcept;
    [[nodiscard]] quint64 fullResolutionRetainedBytes() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool autosavePending() const noexcept;
    [[nodiscard]] bool autosaveFailed() const noexcept;
    [[nodiscard]] QString autosaveErrorText() const;
    [[nodiscard]] bool versionDraft() const noexcept;
    [[nodiscard]] QString editBaseCommitId() const;
    [[nodiscard]] QString durableWorkingCommitId() const;
    [[nodiscard]] QString activeVariantId() const;
    [[nodiscard]] QVariantList photoVariants() const;
    [[nodiscard]] bool variantActionsEnabled() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] QString photoId() const;
    [[nodiscard]] QString representationId() const;
    [[nodiscard]] QString title() const;
    [[nodiscard]] QString sourcePath() const;
    /// Read-only interchange projection. Format controllers may serialize the
    /// current stack but remain unable to mutate edit authority directly.
    [[nodiscard]] const BackendGradeStack& gradeStackForInterchange() const noexcept;
    [[nodiscard]] bool
    applyShadowRecipeGradeNodes(const BackendGradeStack& portable_grade_stack, QString* error_text);
    void reportShadowRecipeExported(const QString& file_name);
    [[nodiscard]] QString previewSource() const;
    [[nodiscard]] QString provisionalPreviewSource() const;
    [[nodiscard]] QString beforePreviewSource() const;
    [[nodiscard]] QVariantMap histogram() const;
    [[nodiscard]] QVariantMap beforeHistogram() const;
    [[nodiscard]] QString beforeErrorText() const;
    [[nodiscard]] bool recipeRecoveryRequired() const noexcept;
    [[nodiscard]] QString recipeRecoveryErrorText() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] bool foundationEnabled() const noexcept;
    [[nodiscard]] int foundationWhiteBalanceTemperature() const noexcept;
    [[nodiscard]] int foundationWhiteBalanceTint() const noexcept;
    [[nodiscard]] bool foundationWhiteBalanceAtCameraValue() const noexcept;
    [[nodiscard]] bool foundationWhiteBalanceCameraValueAvailable() const noexcept;
    [[nodiscard]] bool rawDenoiseNodeMaterialized() const noexcept;
    [[nodiscard]] bool rawDenoiseNodeVisible() const noexcept;
    [[nodiscard]] bool foundationAiDenoiseEnabled() const noexcept;
    [[nodiscard]] bool foundationAiDenoiseRequested() const noexcept;
    [[nodiscard]] int foundationAiDenoiseAmount() const noexcept;
    [[nodiscard]] bool foundationAiDenoiseAvailable() const noexcept;
    [[nodiscard]] bool foundationAiDenoiseBusy() const noexcept;
    [[nodiscard]] bool foundationAiDenoiseCanStart() const noexcept;
    [[nodiscard]] bool foundationAiDenoiseCanApply() const noexcept;
    [[nodiscard]] bool foundationAiDenoiseCanCancel() const noexcept;
    [[nodiscard]] QString foundationAiDenoisePhase() const;
    [[nodiscard]] double foundationAiDenoiseProgress() const noexcept;
    [[nodiscard]] QString foundationAiDenoiseStatusText() const;
    [[nodiscard]] bool foundationAiDenoiseNoiseAssessmentBusy() const noexcept;
    [[nodiscard]] QString foundationAiDenoiseNoiseLevel() const;
    [[nodiscard]] int foundationAiDenoiseNoiseScore() const noexcept;
    [[nodiscard]] int foundationAiDenoiseNoiseConfidence() const noexcept;
    [[nodiscard]] QString foundationAiDenoiseNoiseRecommendation() const;
    [[nodiscard]] bool opticsEnabled() const noexcept;
    [[nodiscard]] bool opticsDistortionEnabled() const noexcept;
    [[nodiscard]] bool opticsTcaEnabled() const noexcept;
    [[nodiscard]] bool opticsVignettingEnabled() const noexcept;
    [[nodiscard]] bool opticsAutomaticScale() const noexcept;
    [[nodiscard]] int manualOpticsDistortion() const noexcept;
    [[nodiscard]] int manualOpticsTcaRedCyan() const noexcept;
    [[nodiscard]] int manualOpticsTcaBlueYellow() const noexcept;
    [[nodiscard]] int manualOpticsVignettingAmount() const noexcept;
    [[nodiscard]] int manualOpticsVignettingMidpoint() const noexcept;
    [[nodiscard]] QVariantMap opticsReceipt() const;
    [[nodiscard]] bool opticsManualProfile() const noexcept;
    [[nodiscard]] QString opticsCameraProfile() const;
    [[nodiscard]] QString opticsLensProfile() const;
    [[nodiscard]] QVariantMap selectedLocalMask() const;
    [[nodiscard]] QVariantList localMaskComponents() const;
    [[nodiscard]] int selectedLocalMaskComponentIndex() const noexcept;
    [[nodiscard]] bool maskToolActive() const noexcept;
    [[nodiscard]] QString maskCoverageSource() const;
    [[nodiscard]] bool maskCoverageShowsSelectedComponent() const noexcept;
    [[nodiscard]] bool aiMaskPromptActive() const noexcept;
    [[nodiscard]] bool aiMaskBusy() const noexcept;
    [[nodiscard]] bool aiMaskFaceRegionMode() const noexcept;
    [[nodiscard]] bool aiMaskSemanticMode() const noexcept;
    [[nodiscard]] QString aiMaskSemanticQuery() const;
    [[nodiscard]] int aiMaskFaceRegion() const noexcept;
    [[nodiscard]] QVariantList aiMaskPeople() const;
    [[nodiscard]] int aiMaskSelectedPerson() const noexcept;
    [[nodiscard]] int aiMaskFaceRegionMask() const noexcept;
    [[nodiscard]] bool aiMaskForegroundMode() const noexcept;
    [[nodiscard]] QVariantList aiMaskPromptPoints() const;
    [[nodiscard]] bool aiMaskCanGenerate() const noexcept;
    [[nodiscard]] bool aiMaskHasCandidate() const noexcept;
    [[nodiscard]] QString aiMaskCandidateSource() const;
    [[nodiscard]] bool imageCompletionActive() const noexcept;
    [[nodiscard]] bool imageCompletionBusy() const noexcept;
    [[nodiscard]] bool imageCompletionCanGenerate() const noexcept;
    [[nodiscard]] bool imageCompletionExecutionAllowed() const noexcept;
    [[nodiscard]] bool imageCompletionHasCandidate() const noexcept;
    [[nodiscard]] QString imageCompletionCandidateSource() const;
    [[nodiscard]] QVariantList imageCompletionBrushPoints() const;
    [[nodiscard]] double imageCompletionBrushRadius() const noexcept;
    [[nodiscard]] bool imageCompletionEraseMode() const noexcept;
    [[nodiscard]] QVariantList imageCompletionRegions() const;
    [[nodiscard]] bool imageCompletionNodeMaterialized() const noexcept;
    [[nodiscard]] bool imageCompletionNodeEnabled() const noexcept;
    [[nodiscard]] bool rawDenoiseExecutionAllowed() const noexcept;
    [[nodiscard]] bool subjectMaskExecutionAllowed() const noexcept;
    [[nodiscard]] bool hasCopiedNodeMask() const noexcept;
    [[nodiscard]] QVariantList retouchSpots() const;
    [[nodiscard]] QVariantList retouchStrokes() const;
    [[nodiscard]] bool retouchNodeEnabled() const noexcept;
    [[nodiscard]] QVariantList liquifyStrokes() const;
    [[nodiscard]] bool liquifyNodeEnabled() const noexcept;
    [[nodiscard]] double liquifyBrushRadius() const noexcept;
    [[nodiscard]] double liquifyBrushStrength() const noexcept;
    [[nodiscard]] double liquifyBrushHardness() const noexcept;
    [[nodiscard]] int liquifyBrushMode() const noexcept;
    [[nodiscard]] bool liquifyCanReconstruct() const noexcept;
    [[nodiscard]] QVariantMap photoGeometry() const;
    [[nodiscard]] bool autoGeometryBusy() const noexcept;
    [[nodiscard]] bool autoGeometryCanAnalyze() const noexcept;
    [[nodiscard]] bool autoGeometryHasProposal() const noexcept;
    [[nodiscard]] bool autoGeometryPreviewing() const noexcept;
    [[nodiscard]] int autoGeometryConfidence() const noexcept;
    [[nodiscard]] double autoGeometrySuggestedStraighten() const noexcept;
    [[nodiscard]] double autoGeometrySuggestedVertical() const noexcept;
    [[nodiscard]] double autoGeometrySuggestedHorizontal() const noexcept;
    [[nodiscard]] int autoGeometrySupportingLines() const noexcept;
    [[nodiscard]] QString autoGeometryStatusText() const;
    [[nodiscard]] bool canvasNodeMaterialized() const noexcept;
    [[nodiscard]] bool canvasNodeEnabled() const noexcept;
    [[nodiscard]] bool cropToolActive() const noexcept;
    [[nodiscard]] QVariantList gradeNodes() const;
    [[nodiscard]] QVariantList sharedGradeNodes() const;
    [[nodiscard]] int selectedGradeNodeIndex() const noexcept;
    [[nodiscard]] QString selectedGradeNodeId() const;
    [[nodiscard]] bool hasSelectedGradeNode() const noexcept;
    [[nodiscard]] bool foundationSelected() const noexcept;
    [[nodiscard]] bool rawDenoiseSelected() const noexcept;
    [[nodiscard]] QString selectedRecipeNodeKind() const;
    [[nodiscard]] bool retouchNodeMaterialized() const noexcept;
    [[nodiscard]] bool liquifyNodeMaterialized() const noexcept;
    [[nodiscard]] bool canAddGradeNode() const noexcept;
    [[nodiscard]] bool canDeleteGradeNode() const noexcept;
    [[nodiscard]] bool canMoveGradeNodeUp() const noexcept;
    [[nodiscard]] bool canMoveGradeNodeDown() const noexcept;
    [[nodiscard]] bool gradeNodeEnabled() const noexcept;
    [[nodiscard]] double gradeNodeStrength() const noexcept;
    [[nodiscard]] double exposureStops() const noexcept;
    [[nodiscard]] double contrastFactor() const noexcept;
    [[nodiscard]] double whiteBalanceTemperature() const noexcept;
    [[nodiscard]] double whiteBalanceTint() const noexcept;
    [[nodiscard]] double saturationFactor() const noexcept;
    [[nodiscard]] quint64 parameterRevision() const noexcept;
    [[nodiscard]] QAbstractItemModel* toneCurvePoints() noexcept;
    [[nodiscard]] QVariantList pointColors() const;
    [[nodiscard]] QVariantList colorWarperControlPoints() const;
    [[nodiscard]] int selectedPointColorIndex() const noexcept;
    [[nodiscard]] bool pointColorScopeActive() const noexcept;
    [[nodiscard]] bool pointColorScopeAvailable() const noexcept;
    [[nodiscard]] bool pointColorPickerActive() const noexcept;
    [[nodiscard]] bool retouchPickerActive() const noexcept;
    [[nodiscard]] int retouchCreationMode() const noexcept;
    [[nodiscard]] int retouchBrushRadius() const noexcept;
    [[nodiscard]] double retouchBrushFeather() const noexcept;
    [[nodiscard]] double retouchBrushStrength() const noexcept;
    [[nodiscard]] bool retouchSourceAligned() const noexcept;
    [[nodiscard]] bool retouchSourcePicking() const noexcept;
    [[nodiscard]] bool retouchSourceSampled() const noexcept;
    [[nodiscard]] QVariantMap retouchSampledSource() const;
    [[nodiscard]] bool whiteBalancePickerActive() const noexcept;
    [[nodiscard]] bool rawWhiteBalancePickerActive() const noexcept;
    [[nodiscard]] bool hasToneCurve() const noexcept;
    [[nodiscard]] bool toneCurveEditable() const noexcept;
    [[nodiscard]] QString lutResourceId() const;
    [[nodiscard]] QString lutTitle() const;
    [[nodiscard]] bool hasLut() const noexcept;
    [[nodiscard]] double lutIntensity() const noexcept;
    [[nodiscard]] QAbstractItemModel* versions() noexcept;

    void setGradeNodeEnabled(bool enabled);
    void setGradeNodeStrength(double strength);
    void setExposureStops(double value);
    void setContrastFactor(double value);
    void setWhiteBalanceTemperature(double value);
    void setWhiteBalanceTint(double value);
    void setSaturationFactor(double value);
    void setLutIntensity(double value);
    void setFoundationEnabled(bool enabled);
    void setFoundationWhiteBalanceTemperature(int temperature_kelvin);
    void setFoundationWhiteBalanceTint(int tint);
    void setFoundationAiDenoiseEnabled(bool enabled);
    void setRawDenoiseNodeVisible(bool visible);
    void setFoundationAiDenoiseAmount(int amount_percent);
    void setCanvasNodeEnabled(bool enabled);
    void setOpticsEnabled(bool enabled);
    void setOpticsDistortionEnabled(bool enabled);
    void setOpticsTcaEnabled(bool enabled);
    void setOpticsVignettingEnabled(bool enabled);
    void setOpticsAutomaticScale(bool enabled);
    void setManualOpticsDistortion(int value);
    void setManualOpticsTcaRedCyan(int value);
    void setManualOpticsTcaBlueYellow(int value);
    void setManualOpticsVignettingAmount(int value);
    void setManualOpticsVignettingMidpoint(int value);

    Q_INVOKABLE bool openPhoto(
        const QString& photo_id,
        const QString& representation_id,
        const QString& source_path,
        const QString& title,
        const QString& provisional_preview_source = {}
    );
    Q_INVOKABLE void closePhoto();
    Q_INVOKABLE void resetIncompatibleRecipe();
    Q_INVOKABLE void selectGradeNode(int index);
    Q_INVOKABLE void addGradeNode();
    Q_INVOKABLE void duplicateSelectedGradeNode();
    Q_INVOKABLE void refreshSharedGradeNodes();
    Q_INVOKABLE void publishSelectedGradeNode(const QString& label);
    Q_INVOKABLE void insertSharedGradeNode(const QString& layer_id);
    Q_INVOKABLE void deleteSelectedGradeNode();
    Q_INVOKABLE void moveSelectedGradeNode(int destination_index);
    // destination: 0 = selected Grade Node, 1 = one newly inserted Grade Node.
    // The new-node path creates, attaches, selects, and records one undo step
    // inside the controller rather than asking QML to chain mutations.
    Q_INVOKABLE bool createLocalMask(int kind, int destination);
    Q_INVOKABLE bool addLocalMaskComponent(int kind, int operation);
    Q_INVOKABLE void selectLocalMaskComponent(int index);
    Q_INVOKABLE void setSelectedLocalMaskComponentEnabled(bool enabled);
    Q_INVOKABLE void setSelectedLocalMaskComponentOperation(int operation);
    Q_INVOKABLE void removeSelectedLocalMaskComponent();
    Q_INVOKABLE void setSelectedLocalMask(int kind);
    Q_INVOKABLE void copySelectedLocalMask();
    Q_INVOKABLE void pasteSelectedLocalMask();
    Q_INVOKABLE void setSelectedLocalMaskValue(const QString& key, double value);
    Q_INVOKABLE void
    setSelectedLocalMaskPoint(const QString& point, double normalized_x, double normalized_y);
    Q_INVOKABLE void appendSelectedLocalMaskBrushStroke(const QVariantList& points);
    Q_INVOKABLE void clearSelectedLocalMaskBrush();
    Q_INVOKABLE void resetSelectedLocalMask();
    Q_INVOKABLE void setSelectedLocalMaskLeafInverted(bool inverted);
    Q_INVOKABLE void setSelectedLocalMaskInverted(bool inverted);
    Q_INVOKABLE void setMaskToolActive(bool active);
    void setMaskCoverageShowsSelectedComponent(bool selected_component);
    Q_INVOKABLE bool beginAiMaskPrompt();
    Q_INVOKABLE bool beginAiFaceMaskPrompt();
    Q_INVOKABLE bool beginAiSemanticMask(const QString& query);
    Q_INVOKABLE bool beginAiMaskPromptForOperation(int operation, bool prefer_current_node);
    Q_INVOKABLE bool beginAiFaceMaskPromptForOperation(int operation, bool prefer_current_node);
    Q_INVOKABLE bool
    beginAiSemanticMaskForOperation(const QString& query, int operation, bool prefer_current_node);
    Q_INVOKABLE void setAiMaskForegroundMode(bool foreground);
    Q_INVOKABLE void setAiMaskFaceRegion(int region);
    Q_INVOKABLE void setAiMaskSelectedPerson(int person_index);
    Q_INVOKABLE void toggleAiMaskFaceRegion(int region, bool selected);
    Q_INVOKABLE void
    addAiMaskPromptPoint(double normalized_x, double normalized_y, bool foreground);
    Q_INVOKABLE void undoAiMaskPromptPoint();
    Q_INVOKABLE void clearAiMaskPromptPoints();
    Q_INVOKABLE void generateAiMask();
    Q_INVOKABLE void applyAiMaskCandidate();
    Q_INVOKABLE void cancelAiMaskPrompt();
    Q_INVOKABLE bool beginImageCompletion();
    Q_INVOKABLE quint32 beginImageCompletionStroke();
    Q_INVOKABLE void
    addImageCompletionBrushPoint(double normalized_x, double normalized_y, quint32 stroke_id);
    Q_INVOKABLE void undoImageCompletionStroke();
    Q_INVOKABLE void clearImageCompletionSelection();
    Q_INVOKABLE void generateImageCompletion();
    Q_INVOKABLE void retryImageCompletion();
    Q_INVOKABLE void applyImageCompletionCandidate();
    Q_INVOKABLE void cancelImageCompletion();
    Q_INVOKABLE void setImageCompletionBrushRadius(double radius);
    Q_INVOKABLE void setImageCompletionEraseMode(bool erase);
    Q_INVOKABLE void setImageCompletionExecutionAllowed(bool allowed);
    Q_INVOKABLE void setImageCompletionNodeEnabled(bool enabled);
    Q_INVOKABLE void setImageCompletionRegionEnabled(int index, bool enabled);
    Q_INVOKABLE void setImageCompletionRegionStrength(int index, double strength);
    Q_INVOKABLE void removeImageCompletionRegion(int index);
    Q_INVOKABLE void startFoundationAiDenoise();
    Q_INVOKABLE void cancelFoundationAiDenoise();
    Q_INVOKABLE void setRetouchPickerActive(bool active);
    Q_INVOKABLE void setRetouchCreationMode(int mode);
    Q_INVOKABLE void setRetouchBrushRadius(int radius_level_zero_pixels);
    Q_INVOKABLE void setRetouchBrushFeather(double feather);
    Q_INVOKABLE void setRetouchBrushStrength(double strength);
    Q_INVOKABLE void adjustRetouchBrushRadius(int direction);
    Q_INVOKABLE void setRetouchSourceAligned(bool aligned);
    Q_INVOKABLE void setRetouchSourcePicking(bool picking);
    Q_INVOKABLE void setRetouchSourceFromPreview(double normalized_x, double normalized_y);
    Q_INVOKABLE void moveRetouchSourceFromPreview(double normalized_x, double normalized_y);
    Q_INVOKABLE void clearRetouchSource();
    Q_INVOKABLE void addRetouchSpotFromPreview(
        double normalized_x,
        double normalized_y,
        const QString& preview_generation,
        int level_zero_width,
        int level_zero_height
    );
    Q_INVOKABLE void addRetouchStrokeFromPreview(
        const QVariantList& points,
        const QString& preview_generation,
        int level_zero_width,
        int level_zero_height
    );
    Q_INVOKABLE void setRetouchSpotCenter(int index, double normalized_x, double normalized_y);
    Q_INVOKABLE void setRetouchSpotRadius(int index, int radius_level_zero_pixels);
    Q_INVOKABLE void setRetouchSpotMode(int index, int mode);
    Q_INVOKABLE void setRetouchSpotFeather(int index, double feather);
    Q_INVOKABLE void setRetouchSpotStrength(int index, double strength);
    Q_INVOKABLE void
    setRetouchSpotSourceOffset(int index, double offset_x_radii, double offset_y_radii);
    Q_INVOKABLE void setRetouchSpotSourceTransform(
        int index,
        double rotation_degrees,
        double scale,
        bool flip_horizontal,
        bool flip_vertical
    );
    Q_INVOKABLE void removeRetouchSpot(int index);
    Q_INVOKABLE void setRetouchStrokeRadius(int index, int radius_level_zero_pixels);
    Q_INVOKABLE void setRetouchStrokeMode(int index, int mode);
    Q_INVOKABLE void setRetouchStrokeFeather(int index, double feather);
    Q_INVOKABLE void setRetouchStrokeStrength(int index, double strength);
    Q_INVOKABLE void
    setRetouchStrokeSourceOffset(int index, double offset_x_radii, double offset_y_radii);
    Q_INVOKABLE void setRetouchStrokeSourceTransform(
        int index,
        double rotation_degrees,
        double scale,
        bool flip_horizontal,
        bool flip_vertical
    );
    Q_INVOKABLE void translateRetouchStroke(int index, double normalized_dx, double normalized_dy);
    Q_INVOKABLE void removeRetouchStroke(int index);
    Q_INVOKABLE void clearRetouch();
    void setRetouchNodeEnabled(bool enabled);
    void setLiquifyBrushRadius(double radius);
    void setLiquifyBrushStrength(double strength);
    void setLiquifyBrushHardness(double hardness);
    void setLiquifyBrushMode(int mode);
    void setLiquifyNodeEnabled(bool enabled);
    Q_INVOKABLE void
    addLiquifyStrokeFromPreview(const QVariantList& points, double output_aspect_ratio);
    Q_INVOKABLE bool beginLiquifyLiveStroke();
    Q_INVOKABLE void
    updateLiquifyLiveStrokeFromPreview(const QVariantList& points, double output_aspect_ratio);
    Q_INVOKABLE void finishLiquifyLiveStroke();
    Q_INVOKABLE void cancelLiquifyLiveStroke();
    Q_INVOKABLE void clearLiquify();
    Q_INVOKABLE void rotatePhotoClockwise();
    Q_INVOKABLE void rotatePhotoCounterClockwise();
    Q_INVOKABLE void flipPhotoHorizontally();
    Q_INVOKABLE void flipPhotoVertically();
    Q_INVOKABLE void
    setCenteredPhotoCropAspectRatio(double output_aspect_ratio, double current_output_aspect_ratio);
    Q_INVOKABLE void
    setPhotoCropBounds(double crop_left, double crop_top, double crop_right, double crop_bottom);
    Q_INVOKABLE void setPhotoStraightenDegrees(double degrees);
    Q_INVOKABLE void setPhotoPerspective(double vertical, double horizontal);
    Q_INVOKABLE void analyzeAutoGeometry(int mode);
    Q_INVOKABLE void acceptAutoGeometry();
    Q_INVOKABLE void cancelAutoGeometry();
    Q_INVOKABLE void setCropToolActive(bool active);
    Q_INVOKABLE void resetPhotoGeometry();
    Q_INVOKABLE void resetSelectedAdjustmentSection(const QString& section_key);
    Q_INVOKABLE void beginParameterEdit(const QString& parameter_key);
    Q_INVOKABLE void endParameterEdit(const QString& parameter_key);
    Q_INVOKABLE double parameterValue(const QString& parameter_key) const;
    Q_INVOKABLE void setParameterValue(const QString& parameter_key, double value);
    Q_INVOKABLE void
    setColorGradingWheel(const QString& tonal_range, double hue, double saturation);
    Q_INVOKABLE void setDefringeHueRange(const QString& family, double lower_hue, double upper_hue);
    Q_INVOKABLE double colorMixerValue(int band_index, const QString& component) const;
    Q_INVOKABLE void setColorMixerValue(int band_index, const QString& component, double value);
    Q_INVOKABLE void setColorWarperControlPoint(int index, double a_offset, double b_offset);
    Q_INVOKABLE void resetColorWarper();
    Q_INVOKABLE double selectiveColorValue(int target_index, int component_index) const;
    Q_INVOKABLE void setSelectiveColorValue(int target_index, int component_index, double value);
    Q_INVOKABLE bool selectiveColorRelative() const noexcept;
    Q_INVOKABLE void setSelectiveColorRelative(bool relative);
    Q_INVOKABLE void
    setLutResource(const QString& resource_id, const QString& title, const QString& managed_path);
    Q_INVOKABLE void clearLut();
    Q_INVOKABLE QVariantList opticsProfileCandidates();
    Q_INVOKABLE void applyManualOpticsProfile(const QVariantMap& profile);
    Q_INVOKABLE void setManualOpticsProfile(
        const QString& camera_maker,
        const QString& camera_model,
        const QString& lens_maker,
        const QString& lens_model
    );
    Q_INVOKABLE void clearManualOpticsProfile();
    Q_INVOKABLE void resetOptics();
    Q_INVOKABLE void selectPointColor(int index);
    void setPointColorScopeActive(bool active);
    Q_INVOKABLE void removeSelectedPointColor();
    Q_INVOKABLE void setPointColorPickerActive(bool active);
    Q_INVOKABLE void setWhiteBalancePickerActive(bool active);
    Q_INVOKABLE void setRawWhiteBalancePickerActive(bool active);
    Q_INVOKABLE void setFoundationWhiteBalanceFromSource(double normalized_x, double normalized_y);
    Q_INVOKABLE void autoFoundationWhiteBalance();
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
    Q_INVOKABLE void beginToneCurveGesture(int index);
    Q_INVOKABLE void moveToneCurvePoint(int index, double x, double y);
    Q_INVOKABLE void endToneCurveGesture(int index);
    Q_INVOKABLE void addToneCurvePoint(double x, double y);
    Q_INVOKABLE void removeToneCurvePoint(int index);
    Q_INVOKABLE void resetToneCurve();
    Q_INVOKABLE void resetFoundationWhiteBalance();
    Q_INVOKABLE void addRawDenoiseNode();
    Q_INVOKABLE void removeRawDenoiseNode();
    Q_INVOKABLE void addCanvasNode();
    Q_INVOKABLE void removeCanvasNode();
    Q_INVOKABLE void selectFoundationNode();
    Q_INVOKABLE void selectRawDenoiseNode();
    Q_INVOKABLE void selectRetouchNode();
    Q_INVOKABLE void selectImageCompletionNode();
    Q_INVOKABLE void selectLiquifyNode();
    Q_INVOKABLE void selectCanvasNode();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void resetSelectedGradeNode();
    Q_INVOKABLE void resetAllGradeNodes();
    Q_INVOKABLE void resetAllAdjustments();
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
    Q_INVOKABLE void createVariant(const QString& name);
    Q_INVOKABLE void renameVariant(const QString& variant_id, const QString& name);
    Q_INVOKABLE void activateVariant(const QString& variant_id);
    Q_INVOKABLE void removeVariant(const QString& variant_id);
    Q_INVOKABLE void retryAutosave();
    // An autosave error must not trap the user in the current photo. These
    // methods are only used after an explicit recovery choice in the shell:
    // retry keeps the pending target, while discard intentionally drops only
    // the in-memory working changes before opening that target.
    Q_INVOKABLE void cancelPendingPhotoOpen();
    Q_INVOKABLE bool discardFailedAutosaveAndOpenPendingPhoto();
    // Returns true when the window may close immediately. When an autosave is
    // required it queues the durable working snapshot and emits closeReady.
    Q_INVOKABLE bool prepareToClose();
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
    void previewGeometryChanged();
    void detailTilesChanged();
    void fullResolutionStateChanged();
    void dirtyChanged();
    void autosavePendingChanged();
    void autosaveFailedChanged();
    void autosaveErrorTextChanged();
    void versionDraftChanged();
    void editBaseCommitIdChanged();
    void photoVariantsChanged();
    void variantActionsChanged();
    void historyChanged();
    void closeReady();
    void closeSaveFailed();
    void photoSwitchSaveFailed();
    void sourceIdentityChanged();
    void titleChanged();
    void sourcePathChanged();
    void previewSourceChanged();
    void provisionalPreviewSourceChanged();
    void beforePreviewSourceChanged();
    void histogramChanged();
    void beforeHistogramChanged();
    void beforeErrorTextChanged();
    void recipeRecoveryChanged();
    void statusTextChanged();
    void foundationChanged();
    // Internal recipe-state notification for the fixed AI RAW Denoise node.
    // The public QML properties retain their historical foundation-prefixed
    // names until the desktop API can make a versioned rename.
    void rawAiDenoiseRecipeChanged();
    void foundationAiDenoiseChanged();
    void opticsChanged();
    void opticsReceiptChanged();
    void gradeNodesChanged();
    void sharedGradeNodesChanged();
    void selectedGradeNodeChanged();
    void gradeNodeActionsChanged();
    void gradeNodeEnabledChanged();
    void parametersChanged();
    void nodeMaskClipboardChanged();
    void maskToolActiveChanged();
    void maskCoverageSourceChanged();
    void maskCoverageModeChanged();
    void aiMaskPromptChanged();
    void imageCompletionChanged();
    void toneCurveChanged();
    void pointColorScopeChanged();
    void pointColorPickerActiveChanged();
    void retouchPickerActiveChanged();
    void retouchCreationModeChanged();
    void retouchBrushChanged();
    void retouchSourceChanged();
    void liquifyBrushChanged();
    void whiteBalancePickerActiveChanged();
    void rawWhiteBalancePickerActiveChanged();
    void cropToolActiveChanged();
    void autoGeometryChanged();

  private slots:
    void finishStateTask();
    void finishPreviewTask();
    void finishDetailTask();
    void finishDetailWarmupTask();
    void startPreviewRender();
    void startDetailRender();
    void startDetailWarmup();

  private:
    struct NodeMaskClipboard final {
        QVector<BackendMaskComponent> components;
        bool final_invert = false;
    };

    void applyState(BackendPhotoEditState state);
    void applySubjectMaskState(
        BackendPhotoEditState state,
        const BackendGradeStack& before,
        const QString& target_grade_node_id
    );
    void applyImageCompletionState(BackendPhotoEditState state, const BackendGradeStack& before);
    void setGradeStack(BackendGradeStack grade_stack, const QString& preferred_grade_node_id = {});
    [[nodiscard]] const BackendGradeNode* selectedGradeNode() const noexcept;
    [[nodiscard]] QString gradeNodeHistoryKey(const QString& key) const;
    [[nodiscard]] QString uniqueGradeNodeLabel(const QString& base) const;
    static void
    initializeLocalMaskComponent(BackendMaskComponent& component, int kind, int operation);
    [[nodiscard]] BackendMaskComponent* selectedLocalMaskComponent() noexcept;
    [[nodiscard]] const BackendMaskComponent* selectedLocalMaskComponent() const noexcept;
    void clampSelectedLocalMaskComponent();
    void finishActiveGesture();
    void cancelActivePreview(bool force);
    void clearSessionHistory();
    void recordWorkingTransition(const QString& key, const BackendGradeStack& before);
    void schedulePreview(int delay_ms);
    void requestPresentationCommit();
    void finalizePhotoClose();
    void maybeStartBeforePreview();
    void maybeStartDetailRender();
    void cancelDetailWarmupForRecipeEdit();
    void scheduleDetailRefreshForRecipeEdit(int delay_ms);
    void scheduleDetailWarmup();
    // A pan changes the requested viewport but not the developed pixels
    // already visible on screen. Keep that presentation until its replacement
    // arrives; Recipe/source changes still discard it immediately.
    void invalidateDetailPresentation(bool discard_tiles = true);
    void resetDetailState();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void setStatusMessage(LocalizedUiMessage status);
    void setDirty(bool dirty);
    void setAutosaveFailure(LocalizedUiMessage error);
    void clearAutosaveFailure();
    void scheduleAutosave();
    void startAutosave();
    // Returns true when an edit was made after the snapshot handed to the
    // Catalog. In that case the returned working head becomes the base for a
    // follow-up autosave, but must never replace the newer in-memory stack.
    [[nodiscard]] bool applyAutosavedState(BackendPhotoEditState state);
    void setVersionDraft(bool draft);
    void setEditBaseCommitId(QString commit_id);
    void setPhotoVariants(QString active_variant_id, QVector<BackendPhotoVariant> variants);
    void startStateTask(EditStateTaskKind kind, QFuture<EditStateTaskResult> future);
    [[nodiscard]] EditStateTaskResult completeStateTask();
    [[nodiscard]] bool stateTaskRunning() const noexcept;
    [[nodiscard]] bool stateTaskFutureRunning() const noexcept;
    [[nodiscard]] EditStateTaskKind stateTaskKind() const noexcept;
    [[nodiscard]] bool interactionLocked() const noexcept;
    [[nodiscard]] bool openPendingPhoto();
    void maybeFinishDeferredApplicationClose();
    void setPreviewRunning(EditPreviewKind kind, bool running);
    [[nodiscard]] std::optional<PreviewScopeHueQualifier> selectedPointColorScopeQualifier() const;
    [[nodiscard]] PreviewDisplayScopeAnalysis analyzeCurrentDisplayScope(
        const QByteArray& encoded_preview,
        const std::optional<PreviewScopeHueQualifier>& point_color_qualifier
    );
    void clearPointColorScopeReference() noexcept;
    void refreshCurrentDisplayScope();
    void markHistogramUpdating(EditPreviewKind kind);
    void publishHistogram(
        EditPreviewKind kind,
        const BackendEditPreviewAnalysis& analysis,
        const PreviewDisplayScopeAnalysis& display_scope,
        quint64 generation
    );
    void markHistogramFailed(EditPreviewKind kind);
    void clearHistograms();
    void setDetailRunning(bool running);
    void setFullResolutionState(bool preparing, bool ready, quint64 retained_bytes);
    void emitBusyChange(bool previous_busy);
    void parameterEdited(const QString& key, const BackendGradeStack& before);
    void foundationEdited(const QString& key, const BackendGradeStack& before);
    void rawDenoiseEdited(const QString& key, const BackendGradeStack& before);
    void opticsEdited(const QString& key, const BackendGradeStack& before);
    void notifyParametersChanged();
    void handleMaskSelectionChanged();
    void handleMaskParametersChanged();
    void handleMaskSourceIdentityChanged();
    void invalidateMaskCoverage();
    void handleSelectedLocalMaskMutation();
    void scheduleMaskCoverageRefresh();
    [[nodiscard]] std::optional<EditMaskCoverageRequest>
    currentMaskCoverageRequest(const BackendGradeStack& grade_stack) const;
    [[nodiscard]] MaskCoverageGeneration
    maskCoverageGeneration(const EditPreviewGeneration& preview_generation) const;
    void publishMaskCoverage(
        BackendMaskCoverage coverage,
        const EditPreviewGeneration& preview_generation
    );
    void toneCurveEdited(const QString& key, const BackendGradeStack& before, int preview_delay_ms);
    [[nodiscard]] bool
    acceptParameter(double value, double minimum, double maximum, const char* label_source);

    friend class EditAiCompletionController;
    friend class EditAiMaskController;
    friend class EditAutoGeometryController;
    friend class EditRawFoundationController;

    std::shared_ptr<DesktopBackend> backend_;
    std::shared_ptr<EditPreviewStore> preview_store_;
    std::shared_ptr<EditPreviewPresentationContext> preview_presentation_context_;
    AiPreferences* ai_preferences_ = nullptr;
    EditPersistenceState persistence_state_;
    std::unique_ptr<EditAiCompletionController> image_completion_controller_;
    std::unique_ptr<EditAiMaskController> ai_mask_controller_;
    std::unique_ptr<EditAutoGeometryController> auto_geometry_controller_;
    std::unique_ptr<EditPersistenceTaskCoordinator> persistence_task_coordinator_;
    std::unique_ptr<EditRawFoundationController> raw_foundation_controller_;
    EditVersionModel versions_;
    ToneCurvePointModel tone_curve_points_;
    QFutureWatcher<EditPreviewTaskResult> preview_watcher_;
    QFutureWatcher<EditDetailTaskResult> detail_watcher_;
    QFutureWatcher<EditDetailWarmupTaskResult> detail_warmup_watcher_;
    QTimer preview_debounce_;
    QTimer detail_debounce_;
    QTimer detail_warmup_debounce_;
    SessionEditHistory<BackendGradeStack> history_;
    // Slider/curve gestures render a deliberately smaller proxy so the first
    // useful frame wins over pixel-perfect fidelity. Once every gesture ends,
    // the controller queues the normal edit preview for the settled recipe.
    QSet<QString> active_parameter_gestures_;
    BackendGradeStack grade_stack_;
    std::optional<NodeMaskClipboard> node_mask_clipboard_;
    QVector<BackendSharedGradeNode> shared_grade_nodes_;
    BackendGradeStack committed_grade_stack_;
    QVariantList photo_variants_;
    QString base_commit_id_;
    QString durable_working_commit_id_;
    QString active_variant_id_;
    QString photo_id_;
    QString representation_id_;
    QString source_path_;
    QString title_;
    QString preview_source_;
    QString provisional_preview_source_;
    QString before_preview_source_;
    QString mask_coverage_source_;
    QVariantMap histogram_;
    QVariantMap before_histogram_;
    QVariantMap optics_receipt_;
    LocalizedUiMessage before_error_message_;
    LocalizedUiMessage detail_error_message_;
    LocalizedUiMessage recipe_recovery_message_;
    LocalizedUiMessage status_message_{
        "EditController",
        QT_TRANSLATE_NOOP("EditController", "Open a photo from Review to begin editing"),
    };
    quint64 photo_generation_ = 0;
    quint64 render_revision_ = 0;
    // This advances only for user-visible recipe mutations. It lets an
    // autosave acknowledge the exact snapshot it wrote without overwriting
    // adjustments made while its Catalog transaction was in flight.
    quint64 working_revision_ = 0;
    quint64 settled_render_revision_ = 0;
    quint64 detail_viewport_revision_ = 0;
    quint64 detail_render_token_ = 0;
    quint64 detail_warmup_token_ = 0;
    quint64 preview_render_token_ = 0;
    QElapsedTimer interactive_preview_timing_;
    quint64 interactive_preview_timing_token_ = 0;
    quint64 mask_selection_revision_ = 0;
    int selected_local_mask_component_index_ = 0;
    quint32 detail_full_width_ = 0;
    quint32 detail_full_height_ = 0;
    quint32 level_zero_width_ = 0;
    quint32 level_zero_height_ = 0;
    quint64 detail_retained_bytes_ = 0;
    quint64 full_resolution_retained_bytes_ = 0;
    QVariantList detail_tiles_;
    double detail_center_x_ = 0.5;
    double detail_center_y_ = 0.5;
    std::uint32_t detail_viewport_width_ = 1;
    std::uint32_t detail_viewport_height_ = 1;
    bool active_ = false;
    bool dirty_ = false;
    bool version_draft_ = false;
    bool current_rendering_ = false;
    bool before_rendering_ = false;
    bool detail_mode_ = false;
    bool detail_rendering_ = false;
    bool full_resolution_preparing_ = false;
    bool full_resolution_ready_ = false;
    bool preview_queued_ = false;
    // Precision may become visually hidden before its asynchronous close is
    // complete. Keep the session alive until the exact saved Recipe preview
    // has crossed the durable Library presentation boundary.
    bool presentation_commit_requested_ = false;
    bool before_requested_ = false;
    bool detail_queued_ = false;
    EditPreviewPolicy in_flight_preview_policy_ = EditPreviewPolicy::Settled;
    // The first interactive frame in one gesture is protected from repeated
    // slider samples so the user sees prompt feedback. Once it has presented,
    // later interactive frames may be cancelled in favour of the latest value.
    bool first_interactive_frame_presented_ = false;
    int selected_grade_node_index_ = -1;
    QString selected_recipe_node_kind_ = QStringLiteral("grade");
    int selected_point_color_index_ = -1;
    bool point_color_scope_active_ = false;
    std::optional<PreviewScopeReferenceSelection> point_color_scope_reference_;
    bool point_color_picker_active_ = false;
    bool retouch_picker_active_ = false;
    int retouch_creation_mode_ = 0;
    int retouch_brush_radius_ = 18;
    double retouch_brush_feather_ = 0.28;
    double retouch_brush_strength_ = 1.0;
    bool retouch_source_aligned_ = true;
    bool retouch_source_picking_ = false;
    std::optional<QPointF> retouch_source_anchor_;
    std::optional<QPointF> retouch_aligned_source_offset_radii_;
    double liquify_brush_radius_ = 0.08;
    double liquify_brush_strength_ = 0.5;
    double liquify_brush_hardness_ = 0.5;
    int liquify_brush_mode_ = 0;
    // When the settled texture cannot accept another local mesh immediately,
    // both Push and Reconstruct use this one provisional Recipe lifecycle.
    // The entire ordered stroke remains preview-only until release.
    std::optional<BackendGradeStack> liquify_live_before_;
    qsizetype liquify_live_index_ = -1;
    int liquify_live_kind_ = -1;
    bool white_balance_picker_active_ = false;
    bool raw_white_balance_picker_active_ = false;
    bool crop_tool_active_ = false;
    bool mask_tool_active_ = false;
    bool mask_coverage_shows_selected_component_ = true;
    bool mask_coverage_refresh_pending_ = false;
    quint64 parameter_revision_ = 0;
};

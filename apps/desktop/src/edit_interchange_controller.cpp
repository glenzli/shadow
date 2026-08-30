#include "edit_interchange_controller.hpp"

#include "edit_controller.hpp"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QVariantMap>

#include <utility>

namespace {

constexpr qint64 MAXIMUM_XMP_BYTES = 4 * 1024 * 1024;

[[nodiscard]] QString number(const double value) {
    return QLocale().toString(value, 'f', 2);
}

} // namespace

EditInterchangeController::EditInterchangeController(
    EditController& editor,
    QObject* const parent
) : QObject(parent), editor_(editor) {
    connect(
        &editor_,
        &EditController::activeChanged,
        this,
        &EditInterchangeController::previewChanged
    );
    connect(
        &editor_,
        &EditController::stateBusyChanged,
        this,
        &EditInterchangeController::previewChanged
    );
    connect(
        &editor_,
        &EditController::gradeNodeActionsChanged,
        this,
        &EditInterchangeController::previewChanged
    );
}

bool EditInterchangeController::ready() const noexcept {
    return ready_;
}

bool EditInterchangeController::canApply() const noexcept {
    return ready_ && preview_.canApply() && editor_.active() && editor_.canAddGradeNode()
           && !editor_.stateBusy();
}

QString EditInterchangeController::sourceName() const {
    return source_name_;
}

QString EditInterchangeController::processVersion() const {
    return preview_.process_version;
}

QVariantList EditInterchangeController::mappedAdjustments() const {
    QVariantList values;
    values.reserve(preview_.adjustments.size());
    for (const auto& adjustment : preview_.adjustments) {
        values.push_back(
            QVariantMap{
                {QStringLiteral("sourceName"), adjustment.source_name},
                {QStringLiteral("sourceValue"), number(adjustment.source_value)},
                {QStringLiteral("targetName"), targetName(adjustment.target)},
                {QStringLiteral("targetValue"), targetValue(adjustment)},
            }
        );
    }
    return values;
}

QVariantList EditInterchangeController::ignoredFields() const {
    QVariantList values;
    values.reserve(preview_.ignored_fields.size());
    for (const auto& field : preview_.ignored_fields) {
        values.push_back(
            QVariantMap{
                {QStringLiteral("sourceName"), field.source_name},
                {QStringLiteral("reason"), ignoredReason(field.reason)},
            }
        );
    }
    return values;
}

QVariantList EditInterchangeController::invalidFields() const {
    QVariantList values;
    values.reserve(preview_.invalid_fields.size());
    for (const auto& field : preview_.invalid_fields) {
        QString problem;
        if (field.problem == QStringLiteral("conflicting values")) {
            problem = tr("The file contains different values for the same field.");
        } else if (field.problem == QStringLiteral("not a finite number")) {
            problem = tr("The value is not a valid finite number.");
        } else {
            problem = tr("The value is outside the supported range.");
        }
        values.push_back(
            QVariantMap{
                {QStringLiteral("sourceName"), field.source_name},
                {QStringLiteral("sourceValue"), field.source_value},
                {QStringLiteral("problem"), problem},
            }
        );
    }
    return values;
}

QString EditInterchangeController::errorText() const {
    if (!preview_error_.isEmpty()) {
        return preview_error_;
    }
    if (!preview_.document_error.isEmpty()) {
        return tr("This is not a readable XMP document: %1").arg(preview_.document_error);
    }
    if (ready_ && preview_.adjustments.isEmpty() && preview_.invalid_fields.isEmpty()) {
        return tr("No supported Camera Raw develop adjustments were found.");
    }
    return {};
}

QString EditInterchangeController::applyErrorText() const {
    return apply_error_;
}

void EditInterchangeController::clear() {
    source_name_.clear();
    preview_ = {};
    preview_error_.clear();
    ready_ = false;
    setApplyError({});
    emit previewChanged();
}

void EditInterchangeController::previewXmp(const QUrl& file_url) {
    source_name_.clear();
    preview_ = {};
    preview_error_.clear();
    ready_ = true;
    setApplyError({});

    const QString path = file_url.toLocalFile();
    const QFileInfo info(path);
    source_name_ = info.fileName();
    if (path.isEmpty() || !info.isFile()) {
        preview_error_ = tr("The selected file is unavailable.");
        emit previewChanged();
        return;
    }
    if (info.size() > MAXIMUM_XMP_BYTES) {
        preview_error_ = tr("The XMP file is larger than 4 MB.");
        emit previewChanged();
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        preview_error_ = tr("The selected file could not be opened.");
        emit previewChanged();
        return;
    }
    preview_ = parseXmpDevelopImport(file.readAll());
    emit previewChanged();
}

bool EditInterchangeController::applyXmp() {
    setApplyError({});
    if (!preview_.canApply()) {
        setApplyError(tr("Resolve the XMP preview before importing."));
        return false;
    }
    if (!editor_.active()) {
        setApplyError(tr("Open a photo before importing XMP adjustments."));
        return false;
    }
    if (editor_.stateBusy()) {
        setApplyError(tr("Wait for the current edit operation to finish."));
        return false;
    }
    if (!editor_.canAddGradeNode()) {
        setApplyError(tr("This photo already contains the maximum of 16 Grade Nodes."));
        return false;
    }

    const QString previous_node_id = editor_.selectedGradeNodeId();
    editor_.addGradeNode();
    if (!editor_.hasSelectedGradeNode() || editor_.selectedGradeNodeId().isEmpty()
        || editor_.selectedGradeNodeId() == previous_node_id) {
        setApplyError(tr("Shadow could not create the destination Grade Node."));
        return false;
    }

    for (const auto& adjustment : preview_.adjustments) {
        switch (adjustment.target) {
        case XmpDevelopTarget::ExposureStops:
            editor_.setExposureStops(adjustment.target_value);
            break;
        case XmpDevelopTarget::ContrastFactor:
            editor_.setContrastFactor(adjustment.target_value);
            break;
        case XmpDevelopTarget::SaturationFactor:
            editor_.setSaturationFactor(adjustment.target_value);
            break;
        case XmpDevelopTarget::Highlights:
            editor_.setParameterValue(QStringLiteral("highlights"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Shadows:
            editor_.setParameterValue(QStringLiteral("shadows"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Whites:
            editor_.setParameterValue(QStringLiteral("whites"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Blacks:
            editor_.setParameterValue(QStringLiteral("blacks"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Texture:
            editor_.setParameterValue(QStringLiteral("texture"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Clarity:
            editor_.setParameterValue(QStringLiteral("clarity"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Dehaze:
            editor_.setParameterValue(QStringLiteral("dehaze"), adjustment.target_value);
            break;
        case XmpDevelopTarget::Vibrance:
            editor_.setParameterValue(QStringLiteral("vibrance"), adjustment.target_value);
            break;
        }
    }
    emit xmpApplied();
    return true;
}

QString EditInterchangeController::targetName(const XmpDevelopTarget target) {
    switch (target) {
    case XmpDevelopTarget::ExposureStops:
        return tr("Exposure");
    case XmpDevelopTarget::ContrastFactor:
        return tr("Contrast");
    case XmpDevelopTarget::Highlights:
        return tr("Highlights");
    case XmpDevelopTarget::Shadows:
        return tr("Shadows");
    case XmpDevelopTarget::Whites:
        return tr("Whites");
    case XmpDevelopTarget::Blacks:
        return tr("Blacks");
    case XmpDevelopTarget::Texture:
        return tr("Texture");
    case XmpDevelopTarget::Clarity:
        return tr("Clarity");
    case XmpDevelopTarget::Dehaze:
        return tr("Dehaze");
    case XmpDevelopTarget::Vibrance:
        return tr("Vibrance");
    case XmpDevelopTarget::SaturationFactor:
        return tr("Chroma");
    }
    return {};
}

QString EditInterchangeController::targetValue(const XmpDevelopAdjustment& adjustment) {
    if (adjustment.target == XmpDevelopTarget::ExposureStops) {
        return tr("%1 EV").arg(number(adjustment.target_value));
    }
    if (adjustment.target == XmpDevelopTarget::ContrastFactor
        || adjustment.target == XmpDevelopTarget::SaturationFactor) {
        return tr("%1×").arg(number(adjustment.target_value));
    }
    return tr("%1%").arg(number(adjustment.target_value * 100.0));
}

QString EditInterchangeController::ignoredReason(const XmpDevelopIgnoredReason reason) {
    switch (reason) {
    case XmpDevelopIgnoredReason::AbsoluteWhiteBalance:
        return tr("Absolute source white balance has no reliable Grade Node equivalent.");
    case XmpDevelopIgnoredReason::StructuredAdjustment:
        return tr("This structured adjustment is outside the controlled mapping.");
    case XmpDevelopIgnoredReason::OutsideControlledMapping:
        return tr("This field is outside the controlled mapping.");
    }
    return {};
}

void EditInterchangeController::setApplyError(QString error) {
    if (apply_error_ == error) {
        return;
    }
    apply_error_ = std::move(error);
    emit applyErrorChanged();
}

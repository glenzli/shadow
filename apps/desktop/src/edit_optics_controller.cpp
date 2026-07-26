#include "edit_controller.hpp"

#include <cstdint>
#include <exception>
#include <initializer_list>

namespace {

constexpr int OPTICS_PREVIEW_THROTTLE_MS = 16;

[[nodiscard]] LocalizedUiMessage optics_message(
    const char* const source,
    const std::initializer_list<LocalizedUiArgument> arguments = {}
) {
    return {"EditController", source, arguments};
}

[[nodiscard]] bool bounded_profile_int(
    const QVariantMap& profile,
    const QString& key,
    const int minimum,
    const int maximum,
    int* const output
) {
    bool converted = false;
    const int value = profile.value(key).toInt(&converted);
    if (!converted || value < minimum || value > maximum) {
        return false;
    }
    *output = value;
    return true;
}

} // namespace

bool EditController::opticsEnabled() const noexcept { return grade_stack_.optics.enabled; }
bool EditController::opticsDistortionEnabled() const noexcept {
    return grade_stack_.optics.correct_distortion;
}
bool EditController::opticsTcaEnabled() const noexcept {
    return grade_stack_.optics.correct_tca;
}
bool EditController::opticsVignettingEnabled() const noexcept {
    return grade_stack_.optics.correct_vignetting;
}
bool EditController::opticsAutomaticScale() const noexcept {
    return grade_stack_.optics.automatic_scale;
}
int EditController::manualOpticsDistortion() const noexcept {
    return grade_stack_.optics.manual_distortion;
}
int EditController::manualOpticsTcaRedCyan() const noexcept {
    return grade_stack_.optics.manual_tca_red_cyan;
}
int EditController::manualOpticsTcaBlueYellow() const noexcept {
    return grade_stack_.optics.manual_tca_blue_yellow;
}
int EditController::manualOpticsVignettingAmount() const noexcept {
    return grade_stack_.optics.manual_vignetting_amount;
}
int EditController::manualOpticsVignettingMidpoint() const noexcept {
    return grade_stack_.optics.manual_vignetting_midpoint;
}
QVariantMap EditController::opticsReceipt() const { return optics_receipt_; }
bool EditController::opticsManualProfile() const noexcept {
    return !grade_stack_.optics.camera_profile_model.isEmpty()
        && !grade_stack_.optics.lens_profile_model.isEmpty();
}
QString EditController::opticsCameraProfile() const {
    return grade_stack_.optics.camera_profile_model;
}
QString EditController::opticsLensProfile() const {
    return grade_stack_.optics.lens_profile_model;
}

void EditController::setOpticsEnabled(const bool enabled) {
    // A Lensfun profile is one coherent correction, not four unrelated
    // user-facing filters.  When a profile is enabled, ask it for every
    // calibrated correction it can supply; unavailable records remain a
    // harmless no-op and are reported through the receipt.  The individual
    // fields stay in the recipe ABI for now so v1 readers remain stable, but
    // they are no longer an editing choice in the desktop product.
    const bool profile_already_complete = !enabled
        || (grade_stack_.optics.correct_distortion
            && grade_stack_.optics.correct_tca
            && grade_stack_.optics.correct_vignetting
            && grade_stack_.optics.automatic_scale);
    if (!active_ || interactionLocked()
        || (grade_stack_.optics.enabled == enabled && profile_already_complete)) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.enabled = enabled;
    if (enabled) {
        grade_stack_.optics.correct_distortion = true;
        grade_stack_.optics.correct_tca = true;
        grade_stack_.optics.correct_vignetting = true;
        grade_stack_.optics.automatic_scale = true;
    }
    opticsEdited(QStringLiteral("profile"), before);
}

void EditController::setOpticsDistortionEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.correct_distortion == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.correct_distortion = enabled;
    opticsEdited(QStringLiteral("distortion"), before);
}

void EditController::setOpticsTcaEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.correct_tca == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.correct_tca = enabled;
    opticsEdited(QStringLiteral("tca"), before);
}

void EditController::setOpticsVignettingEnabled(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.correct_vignetting == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.correct_vignetting = enabled;
    opticsEdited(QStringLiteral("vignetting"), before);
}

void EditController::setOpticsAutomaticScale(const bool enabled) {
    if (!active_ || interactionLocked() || grade_stack_.optics.automatic_scale == enabled) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.automatic_scale = enabled;
    opticsEdited(QStringLiteral("automatic_scale"), before);
}

void EditController::setManualOpticsDistortion(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_distortion == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_distortion = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_distortion"), before);
}

void EditController::setManualOpticsTcaRedCyan(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_tca_red_cyan == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_tca_red_cyan = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_tca_red_cyan"), before);
}

void EditController::setManualOpticsTcaBlueYellow(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_tca_blue_yellow == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_tca_blue_yellow = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_tca_blue_yellow"), before);
}

void EditController::setManualOpticsVignettingAmount(const int value) {
    if (!active_ || interactionLocked() || value < -100 || value > 100
        || grade_stack_.optics.manual_vignetting_amount == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_vignetting_amount = static_cast<std::int16_t>(value);
    opticsEdited(QStringLiteral("manual_vignetting_amount"), before);
}

void EditController::setManualOpticsVignettingMidpoint(const int value) {
    if (!active_ || interactionLocked() || value < 0 || value > 100
        || grade_stack_.optics.manual_vignetting_midpoint == value) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_vignetting_midpoint = static_cast<std::uint8_t>(value);
    opticsEdited(QStringLiteral("manual_vignetting_midpoint"), before);
}

QVariantList EditController::opticsProfileCandidates() {
    if (!active_ || photo_id_.isEmpty() || source_path_.isEmpty()) return {};
    try {
        return backend_->opticsProfileCandidates(photo_id_, source_path_);
    } catch (const std::exception& error) {
        setStatusMessage(optics_message(
            QT_TRANSLATE_NOOP("EditController", "Could not read Lensfun profiles · %1"),
            {QString::fromUtf8(error.what())}
        ));
        return {};
    }
}

void EditController::applyManualOpticsProfile(const QVariantMap& profile) {
    if (!active_ || interactionLocked()) {
        return;
    }
    int distortion = 0;
    int tca_red_cyan = 0;
    int tca_blue_yellow = 0;
    int vignetting_amount = 0;
    int vignetting_midpoint = 50;
    const bool valid = bounded_profile_int(
                           profile,
                           QStringLiteral("manualDistortion"),
                           -100,
                           100,
                           &distortion
                       )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualTcaRedCyan"),
            -100,
            100,
            &tca_red_cyan
        )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualTcaBlueYellow"),
            -100,
            100,
            &tca_blue_yellow
        )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualVignettingAmount"),
            -100,
            100,
            &vignetting_amount
        )
        && bounded_profile_int(
            profile,
            QStringLiteral("manualVignettingMidpoint"),
            0,
            100,
            &vignetting_midpoint
        );
    if (!valid) {
        setStatusMessage(optics_message(QT_TRANSLATE_NOOP(
            "EditController", "This local optical profile is invalid")));
        return;
    }
    if (grade_stack_.optics.manual_distortion == distortion
        && grade_stack_.optics.manual_tca_red_cyan == tca_red_cyan
        && grade_stack_.optics.manual_tca_blue_yellow == tca_blue_yellow
        && grade_stack_.optics.manual_vignetting_amount == vignetting_amount
        && grade_stack_.optics.manual_vignetting_midpoint == vignetting_midpoint) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.manual_distortion = static_cast<std::int16_t>(distortion);
    grade_stack_.optics.manual_tca_red_cyan = static_cast<std::int16_t>(tca_red_cyan);
    grade_stack_.optics.manual_tca_blue_yellow = static_cast<std::int16_t>(tca_blue_yellow);
    grade_stack_.optics.manual_vignetting_amount = static_cast<std::int16_t>(vignetting_amount);
    grade_stack_.optics.manual_vignetting_midpoint = static_cast<std::uint8_t>(vignetting_midpoint);
    const QString profile_id = profile.value(QStringLiteral("id")).toString();
    opticsEdited(
        QStringLiteral("manual_profile/%1").arg(profile_id.isEmpty()
            ? QStringLiteral("custom") : profile_id),
        before
    );
    const QString title = profile.value(QStringLiteral("title")).toString().trimmed();
    setStatusMessage(optics_message(
        QT_TRANSLATE_NOOP("EditController", "Applied manual optical profile · %1"),
        {title.isEmpty() ? tr("Custom profile") : title}
    ));
}

void EditController::setManualOpticsProfile(
    const QString& camera_maker,
    const QString& camera_model,
    const QString& lens_maker,
    const QString& lens_model
) {
    if (!active_ || interactionLocked() || camera_model.trimmed().isEmpty()
        || lens_model.trimmed().isEmpty()) return;
    const BackendGradeStack before = grade_stack_;
    // A user-selected profile follows the same all-calibrated-corrections
    // policy as automatic matching.  Manual controls below the profile tab
    // are deliberately additive residual corrections instead.
    grade_stack_.optics.enabled = true;
    grade_stack_.optics.correct_distortion = true;
    grade_stack_.optics.correct_tca = true;
    grade_stack_.optics.correct_vignetting = true;
    grade_stack_.optics.automatic_scale = true;
    grade_stack_.optics.camera_profile_maker = camera_maker.trimmed();
    grade_stack_.optics.camera_profile_model = camera_model.trimmed();
    grade_stack_.optics.lens_profile_maker = lens_maker.trimmed();
    grade_stack_.optics.lens_profile_model = lens_model.trimmed();
    opticsEdited(QStringLiteral("profile"), before);
}

void EditController::clearManualOpticsProfile() {
    if (!active_ || interactionLocked() || !opticsManualProfile()) return;
    const BackendGradeStack before = grade_stack_;
    grade_stack_.optics.camera_profile_maker.clear();
    grade_stack_.optics.camera_profile_model.clear();
    grade_stack_.optics.lens_profile_maker.clear();
    grade_stack_.optics.lens_profile_model.clear();
    opticsEdited(QStringLiteral("profile"), before);
}

void EditController::opticsEdited(
    const QString& key,
    const BackendGradeStack& before
) {
    recordWorkingTransition(QStringLiteral("optics/%1").arg(key), before);
    setFullResolutionState(false, false, 0);
    emit opticsChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    // Geometry remapping is materially more expensive than a scalar Grade
    // adjustment. Coalesce slider samples to one interactive frame instead
    // of scheduling a separate complete render for every mouse move.
    schedulePreview(OPTICS_PREVIEW_THROTTLE_MS);
}

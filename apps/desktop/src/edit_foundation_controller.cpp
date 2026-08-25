#include "edit_controller.hpp"

#include <cstdint>
#include <exception>

namespace {

constexpr int MIN_TEMPERATURE_KELVIN = 2'000;
constexpr int MAX_TEMPERATURE_KELVIN = 25'000;
constexpr int DEFAULT_TEMPERATURE_KELVIN = 5'500;
constexpr int MIN_TINT = -150;
constexpr int MAX_TINT = 150;
constexpr int FOUNDATION_PREVIEW_THROTTLE_MS = 16;
// Raw-white-balance interaction is admitted through the same warm immutable
// source as every other live edit. Coalesce at display cadence; stale work is
// cancelled by the preview scheduler rather than intentionally adding latency
// to each slider sample.
constexpr int RAW_WHITE_BALANCE_PREVIEW_THROTTLE_MS = 16;
constexpr std::uint32_t RAW_WHITE_BALANCE_PICKER_PREVIEW_EDGE = 1'536U;

[[nodiscard]] LocalizedUiMessage raw_white_balance_message(const char* const source) {
    return {"EditController", source};
}

} // namespace

bool EditController::foundationEnabled() const noexcept {
    return grade_stack_.foundation.enabled;
}

int EditController::foundationWhiteBalanceTemperature() const noexcept {
    return static_cast<int>(grade_stack_.foundation.temperature_kelvin);
}

int EditController::foundationWhiteBalanceTint() const noexcept {
    return static_cast<int>(grade_stack_.foundation.tint);
}

bool EditController::foundationWhiteBalanceAtCameraValue() const noexcept {
    return grade_stack_.foundation.raw_white_balance_mode == 0U;
}

bool EditController::foundationWhiteBalanceCameraValueAvailable() const noexcept {
    return grade_stack_.foundation.as_shot_white_balance_available;
}

bool EditController::rawWhiteBalancePickerActive() const noexcept {
    return raw_white_balance_picker_active_;
}

void EditController::setRawWhiteBalancePickerActive(const bool active) {
    if (!active_ || interactionLocked() || raw_white_balance_picker_active_ == active) {
        return;
    }
    raw_white_balance_picker_active_ = active;
    emit rawWhiteBalancePickerActiveChanged();
    if (active) {
        setPointColorPickerActive(false);
        setWhiteBalancePickerActive(false);
        setRetouchPickerActive(false);
    }
}

void EditController::setFoundationWhiteBalanceFromSource(
    const double normalized_x,
    const double normalized_y
) {
    if (!active_ || interactionLocked() || !raw_white_balance_picker_active_) {
        return;
    }
    try {
        const auto picked = backend_->pickRawWhiteBalance(
            photo_id_,
            source_path_,
            base_commit_id_,
            grade_stack_,
            RAW_WHITE_BALANCE_PICKER_PREVIEW_EDGE,
            normalized_x,
            normalized_y
        );
        if (!picked.available) {
            setStatusMessage(raw_white_balance_message(QT_TRANSLATE_NOOP(
                "EditController", "RAW White Balance picker needs the current RAW preview"
            )));
            return;
        }
        const BackendGradeStack before = grade_stack_;
        grade_stack_.foundation.raw_white_balance_mode = 1U;
        grade_stack_.foundation.temperature_kelvin = picked.temperature_kelvin;
        grade_stack_.foundation.tint = picked.tint;
        setRawWhiteBalancePickerActive(false);
        foundationEdited(QStringLiteral("raw_white_balance/picker"), before);
    } catch (const std::exception&) {
        setStatusMessage(raw_white_balance_message(QT_TRANSLATE_NOOP(
            "EditController", "RAW White Balance picker is unavailable"
        )));
    }
}

void EditController::autoFoundationWhiteBalance() {
    if (!active_ || interactionLocked()) {
        return;
    }
    try {
        const auto estimated = backend_->autoRawWhiteBalance(
            photo_id_,
            source_path_,
            base_commit_id_,
            grade_stack_,
            RAW_WHITE_BALANCE_PICKER_PREVIEW_EDGE
        );
        if (!estimated.available) {
            setStatusMessage(raw_white_balance_message(QT_TRANSLATE_NOOP(
                "EditController",
                "Automatic RAW White Balance could not find a reliable neutral area"
            )));
            return;
        }
        const BackendGradeStack before = grade_stack_;
        grade_stack_.foundation.raw_white_balance_mode = 1U;
        grade_stack_.foundation.temperature_kelvin = estimated.temperature_kelvin;
        grade_stack_.foundation.tint = estimated.tint;
        setRawWhiteBalancePickerActive(false);
        foundationEdited(QStringLiteral("raw_white_balance/auto"), before);
    } catch (const std::exception&) {
        setStatusMessage(raw_white_balance_message(QT_TRANSLATE_NOOP(
            "EditController", "Automatic RAW White Balance needs the current RAW preview"
        )));
    }
}

void EditController::setFoundationEnabled(const bool enabled) {
    if (!active_ || interactionLocked()
        || grade_stack_.foundation.enabled == enabled) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.foundation.enabled = enabled;
    foundationEdited(QStringLiteral("enabled"), before);
}

void EditController::setFoundationWhiteBalanceTemperature(
    const int temperature_kelvin
) {
    if (!active_ || interactionLocked()
        || temperature_kelvin < MIN_TEMPERATURE_KELVIN
        || temperature_kelvin > MAX_TEMPERATURE_KELVIN
        || (grade_stack_.foundation.raw_white_balance_mode == 1U
            && grade_stack_.foundation.temperature_kelvin
                   == static_cast<std::uint32_t>(temperature_kelvin))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.foundation.raw_white_balance_mode = 1U;
    grade_stack_.foundation.temperature_kelvin =
        static_cast<std::uint32_t>(temperature_kelvin);
    foundationEdited(QStringLiteral("raw_white_balance/temperature"), before);
}

void EditController::setFoundationWhiteBalanceTint(const int tint) {
    if (!active_ || interactionLocked() || tint < MIN_TINT || tint > MAX_TINT
        || (grade_stack_.foundation.raw_white_balance_mode == 1U
            && grade_stack_.foundation.tint == static_cast<std::int16_t>(tint))) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.foundation.raw_white_balance_mode = 1U;
    grade_stack_.foundation.tint = static_cast<std::int16_t>(tint);
    foundationEdited(QStringLiteral("raw_white_balance/tint"), before);
}

void EditController::resetFoundationWhiteBalance() {
    if (!active_ || interactionLocked()
        || grade_stack_.foundation.raw_white_balance_mode == 0U) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.foundation.raw_white_balance_mode = 0U;
    grade_stack_.foundation.temperature_kelvin =
        grade_stack_.foundation.as_shot_white_balance_available
            ? grade_stack_.foundation.as_shot_temperature_kelvin
            : DEFAULT_TEMPERATURE_KELVIN;
    grade_stack_.foundation.tint =
        grade_stack_.foundation.as_shot_white_balance_available
            ? grade_stack_.foundation.as_shot_tint
            : 0;
    foundationEdited(QStringLiteral("raw_white_balance/reset"), before);
}

void EditController::foundationEdited(
    const QString& key,
    const BackendGradeStack& before
) {
    recordWorkingTransition(
        QStringLiteral("photo/foundation/%1").arg(key),
        before
    );
    setFullResolutionState(false, false, 0);
    emit foundationChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    schedulePreview(
        key.startsWith(QStringLiteral("raw_white_balance/"))
            ? RAW_WHITE_BALANCE_PREVIEW_THROTTLE_MS
            : FOUNDATION_PREVIEW_THROTTLE_MS
    );
}

void EditController::rawDenoiseEdited(
    const QString& key,
    const BackendGradeStack& before
) {
    recordWorkingTransition(
        QStringLiteral("photo/raw_ai_denoise/%1").arg(key),
        before
    );
    setFullResolutionState(false, false, 0);
    emit rawAiDenoiseRecipeChanged();
    emit foundationAiDenoiseChanged();
    setDirty(version_draft_ || grade_stack_ != committed_grade_stack_);
    schedulePreview(FOUNDATION_PREVIEW_THROTTLE_MS);
}

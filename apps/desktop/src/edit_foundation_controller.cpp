#include "edit_controller.hpp"

#include <cstdint>

namespace {

constexpr int MIN_TEMPERATURE_KELVIN = 2'000;
constexpr int MAX_TEMPERATURE_KELVIN = 25'000;
constexpr int DEFAULT_TEMPERATURE_KELVIN = 5'500;
constexpr int MIN_TINT = -150;
constexpr int MAX_TINT = 150;
constexpr int FOUNDATION_PREVIEW_THROTTLE_MS = 16;
// RAW white balance changes immutable prepared-source provenance, but a warm
// session retains the decoded/denoised camera basis. Keep the interactive
// cadence at roughly 20 fps while the source-stage GPU hand-off is prepared:
// this scheduler coalesces superseded work and always renders the latest value.
// The final gesture value still renders immediately at normal quality.
constexpr int RAW_WHITE_BALANCE_PREVIEW_THROTTLE_MS = 48;

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

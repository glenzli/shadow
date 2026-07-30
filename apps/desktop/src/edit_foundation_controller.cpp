#include "edit_controller.hpp"

#include <cmath>
#include <cstdint>
#include <optional>

namespace {

constexpr std::uint32_t CAMERA_NEUTRAL_MILLIONTHS = 1'000'000;
constexpr double MIN_CAMERA_NEUTRAL_RATIO = 1.0 / 64.0;
constexpr double MAX_CAMERA_NEUTRAL_RATIO = 64.0;
constexpr int FOUNDATION_PREVIEW_THROTTLE_MS = 16;

[[nodiscard]] std::optional<std::uint32_t>
camera_neutral_millionths(const double ratio) {
    if (!std::isfinite(ratio) || ratio < MIN_CAMERA_NEUTRAL_RATIO
        || ratio > MAX_CAMERA_NEUTRAL_RATIO) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(
        std::llround(ratio * static_cast<double>(CAMERA_NEUTRAL_MILLIONTHS))
    );
}

} // namespace

int EditController::foundationWhiteBalanceMode() const noexcept {
    return static_cast<int>(grade_stack_.foundation.raw_white_balance_mode);
}

double EditController::foundationCameraNeutralRed() const noexcept {
    return static_cast<double>(
               grade_stack_.foundation.camera_neutral_red_millionths
           )
        / static_cast<double>(CAMERA_NEUTRAL_MILLIONTHS);
}

double EditController::foundationCameraNeutralBlue() const noexcept {
    return static_cast<double>(
               grade_stack_.foundation.camera_neutral_blue_millionths
           )
        / static_cast<double>(CAMERA_NEUTRAL_MILLIONTHS);
}

void EditController::setFoundationWhiteBalanceMode(const int mode) {
    if (!active_ || interactionLocked() || (mode != 0 && mode != 1)
        || grade_stack_.foundation.raw_white_balance_mode
            == static_cast<std::uint8_t>(mode)) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.foundation.raw_white_balance_mode =
        static_cast<std::uint8_t>(mode);
    foundationEdited(QStringLiteral("raw_white_balance/mode"), before);
}

void EditController::setFoundationCameraNeutralRed(const double value) {
    const auto millionths = camera_neutral_millionths(value);
    if (!active_ || interactionLocked()
        || grade_stack_.foundation.raw_white_balance_mode != 1
        || !millionths.has_value()
        || grade_stack_.foundation.camera_neutral_red_millionths
            == *millionths) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.foundation.camera_neutral_red_millionths = *millionths;
    foundationEdited(QStringLiteral("raw_white_balance/red"), before);
}

void EditController::setFoundationCameraNeutralBlue(const double value) {
    const auto millionths = camera_neutral_millionths(value);
    if (!active_ || interactionLocked()
        || grade_stack_.foundation.raw_white_balance_mode != 1
        || !millionths.has_value()
        || grade_stack_.foundation.camera_neutral_blue_millionths
            == *millionths) {
        return;
    }
    const BackendGradeStack before = grade_stack_;
    grade_stack_.foundation.camera_neutral_blue_millionths = *millionths;
    foundationEdited(QStringLiteral("raw_white_balance/blue"), before);
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
    schedulePreview(FOUNDATION_PREVIEW_THROTTLE_MS);
}

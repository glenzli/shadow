#pragma once

#include <cstdint>
#include <optional>

namespace shadow::desktop {

enum class EditRawFoundationPhase : std::uint8_t {
    Off,
    Checking,
    Available,
    Queued,
    Planning,
    Running,
    Ready,
    Unavailable,
    Cancelled,
    Failed,
};

enum class EditRawFoundationJobAcceptance : std::uint8_t {
    Stale,
    Active,
    Terminal,
};

// Pure generation and lifecycle authority for the desktop RAW Foundation
// surface. Qt workers and translated presentation stay in the controller;
// this owner makes source switches, late completions, cancellation races, and
// non-destructive Recipe intent independently testable.
class EditRawFoundationState final {
  public:
    [[nodiscard]] std::optional<std::uint64_t>
    reset_context(bool active, bool recipe_enabled) noexcept;

    [[nodiscard]] std::uint64_t begin_probe() noexcept;
    [[nodiscard]] bool complete_probe(std::uint64_t context_generation, bool available) noexcept;

    [[nodiscard]] bool
    begin_job(std::uint64_t job_token, std::uint64_t context_generation) noexcept;
    [[nodiscard]] EditRawFoundationJobAcceptance accept_job_status(
        std::uint64_t job_token,
        std::uint64_t context_generation,
        EditRawFoundationPhase phase,
        std::uint16_t completed_basis_points,
        bool cancellation_requested
    ) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> request_cancellation() noexcept;
    [[nodiscard]] bool
    fail_active_job(std::uint64_t job_token, std::uint64_t context_generation) noexcept;

    [[nodiscard]] bool sync_recipe_enabled(bool enabled) noexcept;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] bool runtime_known() const noexcept;
    [[nodiscard]] bool runtime_available() const noexcept;
    [[nodiscard]] bool recipe_enabled() const noexcept;
    [[nodiscard]] bool materialized_ready() const noexcept;
    [[nodiscard]] bool job_busy() const noexcept;
    [[nodiscard]] bool cancellation_requested() const noexcept;
    [[nodiscard]] bool can_start_job() const noexcept;
    [[nodiscard]] std::uint64_t context_generation() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> active_job_token() const noexcept;
    [[nodiscard]] EditRawFoundationPhase phase() const noexcept;
    [[nodiscard]] std::uint16_t completed_basis_points() const noexcept;

  private:
    std::uint64_t context_generation_ = 0;
    std::optional<std::uint64_t> active_job_token_;
    EditRawFoundationPhase phase_ = EditRawFoundationPhase::Off;
    std::uint16_t completed_basis_points_ = 0;
    bool active_ = false;
    bool runtime_known_ = false;
    bool runtime_available_ = false;
    bool recipe_enabled_ = false;
    bool materialized_ready_ = false;
    bool cancellation_requested_ = false;
};

} // namespace shadow::desktop

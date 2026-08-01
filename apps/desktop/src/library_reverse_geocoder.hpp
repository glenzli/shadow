#pragma once

#include "backend/library_types.hpp"

#include <QString>

#include <functional>
#include <memory>
#include <optional>

/// One-at-a-time platform reverse-geocoding boundary.
///
/// Implementations invoke the completion on the caller's UI thread. An empty
/// error accompanies success; failures never invent a partial place result.
class LibraryReverseGeocoder {
  public:
    using Completion = std::function<void(
        std::optional<BackendLibraryPlaceResolutionResult> result,
        QString error
    )>;

    virtual ~LibraryReverseGeocoder() = default;

    [[nodiscard]] virtual bool available() const noexcept = 0;
    virtual void reverseGeocode(
        const BackendLibraryPlaceResolutionCandidate& candidate,
        Completion completion
    ) = 0;
    virtual void cancel() noexcept = 0;
};

/// Creates the native no-key provider when the platform offers one. Other
/// platforms receive a fail-closed unavailable implementation.
[[nodiscard]] std::unique_ptr<LibraryReverseGeocoder> makeSystemLibraryReverseGeocoder();

#include "library_reverse_geocoder.hpp"

#include <utility>

namespace {

class UnavailableLibraryReverseGeocoder final : public LibraryReverseGeocoder {
  public:
    [[nodiscard]] bool available() const noexcept override {
        return false;
    }

    void reverseGeocode(
        const BackendLibraryPlaceResolutionCandidate&,
        Completion completion
    ) override {
        completion(
            std::nullopt,
            QStringLiteral("native reverse geocoding is unavailable on this platform")
        );
    }

    void cancel() noexcept override {}
};

} // namespace

std::unique_ptr<LibraryReverseGeocoder> makeSystemLibraryReverseGeocoder() {
    return std::make_unique<UnavailableLibraryReverseGeocoder>();
}

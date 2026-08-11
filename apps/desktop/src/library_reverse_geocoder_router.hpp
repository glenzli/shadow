#pragma once

#include "library_reverse_geocoder.hpp"

#include <functional>
#include <memory>
#include <vector>

/// One ordered reverse-geocoding route.
///
/// `accepts` owns provider-selection policy such as explicit authorization or
/// geographic coverage. The provider owns only its request lifecycle.
struct LibraryReverseGeocoderRoute final {
    std::unique_ptr<LibraryReverseGeocoder> provider;
    std::function<bool(const BackendLibraryPlaceResolutionCandidate&)> accepts;
};

/// Tries eligible, available providers in order until one resolves the place.
/// This keeps provider policy out of individual network/offline implementations
/// and permits any number of precise providers before the local fallback.
[[nodiscard]] std::unique_ptr<LibraryReverseGeocoder>
makeLibraryReverseGeocoderRouter(std::vector<LibraryReverseGeocoderRoute> routes);

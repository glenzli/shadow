#pragma once

#include "library_reverse_geocoder.hpp"

#include <functional>
#include <memory>

/// Routes to the offline provider by default. When the user explicitly allows
/// Google reverse geocoding, the online provider is preferred and an online
/// failure falls back to the local city index.
[[nodiscard]] std::unique_ptr<LibraryReverseGeocoder> makeLibraryReverseGeocoderRouter(
    std::unique_ptr<LibraryReverseGeocoder> offline,
    std::unique_ptr<LibraryReverseGeocoder> online,
    std::function<bool()> prefer_online
);

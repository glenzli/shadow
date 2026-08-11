#pragma once

#include "library_reverse_geocoder.hpp"

#include <memory>

class MapProviderPreferences;

/// Composes Shadow's local-default location lookup with explicitly authorized
/// AMap and Google precision providers.
[[nodiscard]] std::unique_ptr<LibraryReverseGeocoder>
makeDefaultLibraryReverseGeocoder(MapProviderPreferences* preferences);

#pragma once

#include "library_reverse_geocoder.hpp"

#include <QString>

#include <memory>

/// Returns the packaged city-index path, or the explicit test/developer
/// override from SHADOW_GEONAMES_CITY_INDEX_PATH.
[[nodiscard]] QString defaultGeoNamesCityIndexPath();

/// Creates the offline, city-level GeoNames provider. The index is loaded on
/// first use off the UI thread and retained for subsequent coordinates.
[[nodiscard]] std::unique_ptr<LibraryReverseGeocoder>
makeGeoNamesLibraryReverseGeocoder(const QString& index_path = defaultGeoNamesCityIndexPath());

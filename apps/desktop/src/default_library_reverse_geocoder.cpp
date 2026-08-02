#include "default_library_reverse_geocoder.hpp"

#include "geonames_library_reverse_geocoder.hpp"
#include "google_library_reverse_geocoder.hpp"
#include "library_reverse_geocoder_router.hpp"
#include "map_provider_preferences.hpp"

#include <stdexcept>

std::unique_ptr<LibraryReverseGeocoder>
makeDefaultLibraryReverseGeocoder(MapProviderPreferences* const preferences) {
    if (preferences == nullptr) {
        throw std::invalid_argument("map provider preferences are required");
    }
    return makeLibraryReverseGeocoderRouter(
        makeGeoNamesLibraryReverseGeocoder(),
        makeGoogleLibraryReverseGeocoder(preferences),
        [preferences]() {
            return preferences->googleApiKeyStored()
                   && preferences->googleReverseGeocodingAllowed();
        }
    );
}

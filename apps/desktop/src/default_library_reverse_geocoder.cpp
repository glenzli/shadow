#include "default_library_reverse_geocoder.hpp"

#include "amap_coordinate_transform.hpp"
#include "amap_library_reverse_geocoder.hpp"
#include "geonames_library_reverse_geocoder.hpp"
#include "google_library_reverse_geocoder.hpp"
#include "library_reverse_geocoder_router.hpp"
#include "map_provider_preferences.hpp"

#include <stdexcept>
#include <vector>

std::unique_ptr<LibraryReverseGeocoder>
makeDefaultLibraryReverseGeocoder(MapProviderPreferences* const preferences) {
    if (preferences == nullptr) {
        throw std::invalid_argument("map provider preferences are required");
    }
    std::vector<LibraryReverseGeocoderRoute> routes;
    routes.push_back({
        .provider = makeAmapLibraryReverseGeocoder(preferences),
        .accepts = [preferences](const BackendLibraryPlaceResolutionCandidate& candidate) {
            const double latitude = static_cast<double>(candidate.latitude_e7) / 10'000'000.0;
            const double longitude = static_cast<double>(candidate.longitude_e7) / 10'000'000.0;
            return preferences->amapWebServiceKeyStored()
                   && preferences->amapReverseGeocodingAllowed()
                   && shadow::desktop::maps::amapDomesticCoordinateSupported(
                       latitude,
                       longitude
                   );
        },
    });
    routes.push_back({
        .provider = makeGoogleLibraryReverseGeocoder(preferences),
        .accepts = [preferences](const BackendLibraryPlaceResolutionCandidate&) {
            return preferences->googleApiKeyStored()
                   && preferences->googleReverseGeocodingAllowed();
        },
    });
    routes.push_back({
        .provider = makeGeoNamesLibraryReverseGeocoder(),
        .accepts = [](const BackendLibraryPlaceResolutionCandidate&) { return true; },
    });
    return makeLibraryReverseGeocoderRouter(std::move(routes));
}

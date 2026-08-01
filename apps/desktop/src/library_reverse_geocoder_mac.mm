#include "library_reverse_geocoder.hpp"

#import <CoreLocation/CoreLocation.h>
#import <MapKit/MapKit.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace {

[[nodiscard]] QString qstring(NSString* const value) {
    if (value == nil) {
        return {};
    }
    return QString::fromUtf8(value.UTF8String);
}

struct ReverseGeocoderState final {
    std::uint64_t generation = 0;
    __strong MKReverseGeocodingRequest* request = nil;
    LibraryReverseGeocoder::Completion completion;
};

class AppleLibraryReverseGeocoder final : public LibraryReverseGeocoder {
  public:
    AppleLibraryReverseGeocoder() : state_(std::make_shared<ReverseGeocoderState>()) {}

    ~AppleLibraryReverseGeocoder() override {
        cancel();
    }

    [[nodiscard]] bool available() const noexcept override {
        if (@available(macOS 26.0, *)) {
            return true;
        }
        return false;
    }

    void reverseGeocode(
        const BackendLibraryPlaceResolutionCandidate& candidate,
        Completion completion
    ) override {
        cancel();
        if (!available()) {
            completion(std::nullopt, QStringLiteral("MapKit reverse geocoding is unavailable"));
            return;
        }

        const CLLocationDegrees latitude =
            static_cast<CLLocationDegrees>(candidate.latitude_e7) / 10'000'000.0;
        const CLLocationDegrees longitude =
            static_cast<CLLocationDegrees>(candidate.longitude_e7) / 10'000'000.0;
        CLLocation* const location =
            [[CLLocation alloc] initWithLatitude:latitude longitude:longitude];
        MKReverseGeocodingRequest* const request =
            [[MKReverseGeocodingRequest alloc] initWithLocation:location];
        if (request == nil) {
            completion(std::nullopt, QStringLiteral("MapKit rejected the photo coordinates"));
            return;
        }
        request.preferredLocale = NSLocale.currentLocale;

        const auto state = state_;
        const std::uint64_t generation = ++state->generation;
        state->request = request;
        state->completion = std::move(completion);
        [request getMapItemsWithCompletionHandler:^(NSArray<MKMapItem*>* map_items, NSError* error) {
            if (state->generation != generation) {
                return;
            }
            auto completion = std::move(state->completion);
            state->completion = {};
            state->request = nil;
            if (!completion) {
                return;
            }
            if (error != nil) {
                completion(std::nullopt, qstring(error.localizedDescription));
                return;
            }

            for (MKMapItem* const item in map_items) {
                MKAddressRepresentations* const address = item.addressRepresentations;
                if (address == nil) {
                    continue;
                }
                const QString country_code = qstring(address.regionCode).trimmed();
                const QString country_name = qstring(address.regionName).trimmed();
                if (country_code.isEmpty() && country_name.isEmpty()) {
                    continue;
                }
                QString display_name =
                    qstring([address fullAddressIncludingRegion:YES singleLine:YES]).trimmed();
                if (display_name.isEmpty()) {
                    display_name =
                        qstring([address cityWithContextUsingStyle:
                                             MKAddressRepresentationsContextStyleFull])
                            .trimmed();
                }
                completion(
                    BackendLibraryPlaceResolutionResult{
                        .latitude_e7 = candidate.latitude_e7,
                        .longitude_e7 = candidate.longitude_e7,
                        .country_code = country_code,
                        .country_name = country_name,
                        // Modern MapKit exposes a localized city-with-context string but no
                        // locale-independent administrative-area field. Keep this empty rather
                        // than parsing presentation text into unstable identity.
                        .administrative_area = {},
                        .locality = qstring(address.cityName).trimmed(),
                        .display_name = display_name,
                        .provider_id = QStringLiteral("apple-mapkit"),
                        .provider_version = QStringLiteral("mk-reverse-geocoding-v1"),
                        .locale = qstring(NSLocale.currentLocale.localeIdentifier),
                    },
                    {}
                );
                return;
            }
            completion(
                std::nullopt,
                QStringLiteral("MapKit returned no structured country for the coordinates")
            );
        }];
    }

    void cancel() noexcept override {
        ++state_->generation;
        [state_->request cancel];
        state_->request = nil;
        state_->completion = {};
    }

  private:
    std::shared_ptr<ReverseGeocoderState> state_;
};

} // namespace

std::unique_ptr<LibraryReverseGeocoder> makeSystemLibraryReverseGeocoder() {
    return std::make_unique<AppleLibraryReverseGeocoder>();
}

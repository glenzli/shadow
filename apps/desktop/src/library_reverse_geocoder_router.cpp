#include "library_reverse_geocoder_router.hpp"

#include <QCoreApplication>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

class LibraryReverseGeocoderRouter final : public LibraryReverseGeocoder {
  public:
    explicit LibraryReverseGeocoderRouter(std::vector<LibraryReverseGeocoderRoute> routes) :
        routes_(std::move(routes)) {
        if (routes_.empty()) {
            throw std::invalid_argument("at least one reverse-geocoder route is required");
        }
        for (const LibraryReverseGeocoderRoute& route : routes_) {
            if (!route.provider || !route.accepts) {
                throw std::invalid_argument("reverse-geocoder route dependencies are required");
            }
        }
    }

    ~LibraryReverseGeocoderRouter() override {
        cancel();
    }

    [[nodiscard]] bool available() const noexcept override {
        for (const LibraryReverseGeocoderRoute& route : routes_) {
            if (route.provider->available()) {
                return true;
            }
        }
        return false;
    }

    void reverseGeocode(
        const BackendLibraryPlaceResolutionCandidate& candidate,
        Completion completion
    ) override {
        cancel();
        completion_ = std::move(completion);
        const std::uint64_t request_generation = ++generation_;
        beginRoute(candidate, 0, request_generation, {});
    }

    void cancel() noexcept override {
        ++generation_;
        completion_ = {};
        for (LibraryReverseGeocoderRoute& route : routes_) {
            route.provider->cancel();
        }
    }

  private:
    void beginRoute(
        const BackendLibraryPlaceResolutionCandidate& candidate,
        std::size_t route_index,
        const std::uint64_t request_generation,
        QString previous_error
    ) {
        while (route_index < routes_.size()
               && (!routes_[route_index].accepts(candidate)
                   || !routes_[route_index].provider->available())) {
            ++route_index;
        }
        if (route_index >= routes_.size()) {
            finish(
                request_generation,
                std::nullopt,
                previous_error.isEmpty() ? QCoreApplication::translate(
                                               "LibraryReverseGeocoderRouter",
                                               "No location lookup provider is available."
                                           )
                                         : std::move(previous_error)
            );
            return;
        }
        routes_[route_index].provider->reverseGeocode(
            candidate,
            [this,
             candidate,
             route_index,
             request_generation,
             previous_error = std::move(
                 previous_error
             )](std::optional<BackendLibraryPlaceResolutionResult> result, QString error) {
                if (request_generation != generation_) {
                    return;
                }
                if (result) {
                    finish(request_generation, std::move(result), std::move(error));
                    return;
                }
                beginRoute(
                    candidate,
                    route_index + 1,
                    request_generation,
                    error.isEmpty() ? previous_error : std::move(error)
                );
            }
        );
    }

    void finish(
        const std::uint64_t request_generation,
        std::optional<BackendLibraryPlaceResolutionResult> result,
        QString error
    ) {
        if (request_generation != generation_) {
            return;
        }
        auto completion = std::move(completion_);
        completion_ = {};
        if (completion) {
            completion(std::move(result), std::move(error));
        }
    }

    std::vector<LibraryReverseGeocoderRoute> routes_;
    Completion completion_;
    std::uint64_t generation_ = 0;
};

} // namespace

std::unique_ptr<LibraryReverseGeocoder>
makeLibraryReverseGeocoderRouter(std::vector<LibraryReverseGeocoderRoute> routes) {
    return std::make_unique<LibraryReverseGeocoderRouter>(std::move(routes));
}

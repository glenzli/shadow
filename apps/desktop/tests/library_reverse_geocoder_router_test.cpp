#include "library_reverse_geocoder_router.hpp"

#include <QString>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

class ImmediateProvider final : public LibraryReverseGeocoder {
  public:
    [[nodiscard]] bool available() const noexcept override {
        return available_;
    }

    void
    reverseGeocode(const BackendLibraryPlaceResolutionCandidate&, Completion completion) override {
        ++request_count;
        completion(result, error);
    }

    void cancel() noexcept override {
        ++cancel_count;
    }

    bool available_ = true;
    std::optional<BackendLibraryPlaceResolutionResult> result;
    QString error;
    int request_count = 0;
    int cancel_count = 0;
};

[[nodiscard]] BackendLibraryPlaceResolutionResult resultFrom(const QString& provider_id) {
    return {
        .country_code = QStringLiteral("CN"),
        .country_name = QStringLiteral("China"),
        .administrative_area = QStringLiteral("Shanghai"),
        .locality = QStringLiteral("Shanghai"),
        .display_name = QStringLiteral("Shanghai, China"),
        .provider_id = provider_id,
        .provider_version = QStringLiteral("v1"),
        .locale = QStringLiteral("en"),
    };
}

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "Reverse-geocoder router contract failed: " << message << '\n';
    }
    return condition;
}

struct RoutingFixture final {
    ImmediateProvider* offline = nullptr;
    ImmediateProvider* online = nullptr;
    ImmediateProvider* regional = nullptr;
    std::unique_ptr<LibraryReverseGeocoder> router;
};

[[nodiscard]] RoutingFixture makeFixture(
    const bool prefer_online,
    const bool online_fails,
    const bool regional_accepts = false
) {
    auto offline = std::make_unique<ImmediateProvider>();
    auto online = std::make_unique<ImmediateProvider>();
    auto regional = std::make_unique<ImmediateProvider>();
    auto* const offline_pointer = offline.get();
    auto* const online_pointer = online.get();
    auto* const regional_pointer = regional.get();
    offline->result = resultFrom(QStringLiteral("offline"));
    regional->result = resultFrom(QStringLiteral("regional"));
    if (online_fails) {
        online->error = QStringLiteral("network failed");
    } else {
        online->result = resultFrom(QStringLiteral("online"));
    }
    std::vector<LibraryReverseGeocoderRoute> routes;
    routes.push_back({
        .provider = std::move(regional),
        .accepts = [regional_accepts](const BackendLibraryPlaceResolutionCandidate&) {
            return regional_accepts;
        },
    });
    routes.push_back({
        .provider = std::move(online),
        .accepts = [prefer_online](const BackendLibraryPlaceResolutionCandidate&) {
            return prefer_online;
        },
    });
    routes.push_back({
        .provider = std::move(offline),
        .accepts = [](const BackendLibraryPlaceResolutionCandidate&) { return true; },
    });
    return {
        .offline = offline_pointer,
        .online = online_pointer,
        .regional = regional_pointer,
        .router = makeLibraryReverseGeocoderRouter(std::move(routes)),
    };
}

[[nodiscard]] std::optional<BackendLibraryPlaceResolutionResult>
resolve(LibraryReverseGeocoder& router, QString* error) {
    std::optional<BackendLibraryPlaceResolutionResult> result;
    router.reverseGeocode({}, [&result, error](auto value, QString failure) {
        result = std::move(value);
        *error = std::move(failure);
    });
    return result;
}

} // namespace

int main() {
    QString error;
    auto local = makeFixture(false, false);
    const auto local_result = resolve(*local.router, &error);
    if (!require(
            local_result && local_result->provider_id == QStringLiteral("offline"),
            "offline provider is not the default"
        )
        || !require(
            local.offline->request_count == 1 && local.online->request_count == 0
                && local.regional->request_count == 0,
            "default lookup contacted the online provider"
        )) {
        return EXIT_FAILURE;
    }

    auto precise = makeFixture(true, false);
    error.clear();
    const auto precise_result = resolve(*precise.router, &error);
    if (!require(
            precise_result && precise_result->provider_id == QStringLiteral("online"),
            "explicit precision mode did not select the online provider"
        )
        || !require(
            precise.online->request_count == 1 && precise.offline->request_count == 0
                && precise.regional->request_count == 0,
            "successful online lookup unnecessarily queried offline data"
        )) {
        return EXIT_FAILURE;
    }

    auto fallback = makeFixture(true, true);
    error.clear();
    const auto fallback_result = resolve(*fallback.router, &error);
    if (!require(
            fallback_result && fallback_result->provider_id == QStringLiteral("offline")
                && error.isEmpty(),
            "online failure did not fall back to offline city data"
        )) {
        return EXIT_FAILURE;
    }

    auto regional = makeFixture(true, false, true);
    error.clear();
    const auto regional_result = resolve(*regional.router, &error);
    return require(
               regional_result && regional_result->provider_id == QStringLiteral("regional")
                   && regional.regional->request_count == 1 && regional.online->request_count == 0
                   && regional.offline->request_count == 0,
               "the first eligible regional provider did not own the request"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

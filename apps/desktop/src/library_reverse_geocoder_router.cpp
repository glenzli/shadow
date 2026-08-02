#include "library_reverse_geocoder_router.hpp"

#include <QCoreApplication>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {

class LibraryReverseGeocoderRouter final : public LibraryReverseGeocoder {
  public:
    LibraryReverseGeocoderRouter(
        std::unique_ptr<LibraryReverseGeocoder> offline,
        std::unique_ptr<LibraryReverseGeocoder> online,
        std::function<bool()> prefer_online
    ) :
        offline_(std::move(offline)), online_(std::move(online)),
        prefer_online_(std::move(prefer_online)) {
        if (!offline_ || !online_ || !prefer_online_) {
            throw std::invalid_argument("reverse-geocoder router dependencies are required");
        }
    }

    ~LibraryReverseGeocoderRouter() override {
        cancel();
    }

    [[nodiscard]] bool available() const noexcept override {
        return offline_->available() || online_->available();
    }

    void reverseGeocode(
        const BackendLibraryPlaceResolutionCandidate& candidate,
        Completion completion
    ) override {
        cancel();
        completion_ = std::move(completion);
        const std::uint64_t request_generation = ++generation_;
        if (prefer_online_() && online_->available()) {
            beginOnline(candidate, request_generation);
            return;
        }
        if (offline_->available()) {
            beginOffline(candidate, request_generation);
            return;
        }
        if (online_->available()) {
            beginOnline(candidate, request_generation);
            return;
        }
        finish(
            request_generation,
            std::nullopt,
            QCoreApplication::translate(
                "LibraryReverseGeocoderRouter",
                "No location lookup provider is available."
            )
        );
    }

    void cancel() noexcept override {
        ++generation_;
        completion_ = {};
        offline_->cancel();
        online_->cancel();
    }

  private:
    void beginOnline(
        const BackendLibraryPlaceResolutionCandidate& candidate,
        const std::uint64_t request_generation
    ) {
        online_->reverseGeocode(
            candidate,
            [this, candidate, request_generation](
                std::optional<BackendLibraryPlaceResolutionResult> result,
                QString error
            ) {
                if (request_generation != generation_) {
                    return;
                }
                if (result || !offline_->available()) {
                    finish(request_generation, std::move(result), std::move(error));
                    return;
                }
                beginOffline(candidate, request_generation);
            }
        );
    }

    void beginOffline(
        const BackendLibraryPlaceResolutionCandidate& candidate,
        const std::uint64_t request_generation
    ) {
        offline_->reverseGeocode(
            candidate,
            [this, request_generation](
                std::optional<BackendLibraryPlaceResolutionResult> result,
                QString error
            ) { finish(request_generation, std::move(result), std::move(error)); }
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

    std::unique_ptr<LibraryReverseGeocoder> offline_;
    std::unique_ptr<LibraryReverseGeocoder> online_;
    std::function<bool()> prefer_online_;
    Completion completion_;
    std::uint64_t generation_ = 0;
};

} // namespace

std::unique_ptr<LibraryReverseGeocoder> makeLibraryReverseGeocoderRouter(
    std::unique_ptr<LibraryReverseGeocoder> offline,
    std::unique_ptr<LibraryReverseGeocoder> online,
    std::function<bool()> prefer_online
) {
    return std::make_unique<LibraryReverseGeocoderRouter>(
        std::move(offline),
        std::move(online),
        std::move(prefer_online)
    );
}

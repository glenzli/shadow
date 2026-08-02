#include "review_library_place_resolution_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Library place coordinator contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] bool waitUntil(const std::function<bool()>& condition, const int timeout_ms = 2'000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return condition();
}

class ControlledReverseGeocoder final : public LibraryReverseGeocoder {
  public:
    [[nodiscard]] bool available() const noexcept override {
        return available_;
    }

    void reverseGeocode(
        const BackendLibraryPlaceResolutionCandidate& candidate,
        Completion completion
    ) override {
        ++request_count;
        requested = candidate;
        completion_ = std::move(completion);
    }

    void cancel() noexcept override {
        ++cancel_count;
        completion_ = {};
    }

    void succeed() {
        auto completion = std::move(completion_);
        require(static_cast<bool>(completion), "a provider completion must be pending");
        completion(
            BackendLibraryPlaceResolutionResult{
                .country_code = QStringLiteral("CN"),
                .country_name = QStringLiteral("China"),
                .administrative_area = QStringLiteral("Shanghai"),
                .locality = QStringLiteral("Shanghai"),
                .display_name = QStringLiteral("Shanghai, China"),
                .provider_id = QStringLiteral("test"),
                .provider_version = QStringLiteral("1"),
                .locale = QStringLiteral("en"),
            },
            {}
        );
    }

    void fail(const QString& diagnostic) {
        auto completion = std::move(completion_);
        require(static_cast<bool>(completion), "a provider completion must be pending");
        completion(std::nullopt, diagnostic);
    }

    bool available_ = true;
    int request_count = 0;
    int cancel_count = 0;
    BackendLibraryPlaceResolutionCandidate requested;

  private:
    Completion completion_;
};

void resolvesAfterExplicitStartWithoutBlockingTheCaller() {
    auto provider = std::make_unique<ControlledReverseGeocoder>();
    auto* const controlled = provider.get();
    bool recorded = false;
    BackendLibraryPlaceResolutionResult persisted;
    ReviewLibraryPlaceResolutionCoordinator coordinator(
        {
            .candidates =
                [&recorded](const std::uint32_t) {
                    if (recorded) {
                        return QVector<BackendLibraryPlaceResolutionCandidate>{};
                    }
                    return QVector<BackendLibraryPlaceResolutionCandidate>{
                        {
                            .latitude_e7 = 312'304'000,
                            .longitude_e7 = 1'212'473'000,
                            .photo_count = 2,
                        },
                    };
                },
            .record =
                [&recorded, &persisted](const BackendLibraryPlaceResolutionResult& result) {
                    recorded = true;
                    persisted = result;
                    return BackendRecordLibraryPlaceResolutionStatus::Recorded;
                },
        },
        std::move(provider)
    );
    int place_changes = 0;
    QObject::connect(
        &coordinator,
        &ReviewLibraryPlaceResolutionCoordinator::placesChanged,
        &coordinator,
        [&place_changes]() { ++place_changes; }
    );

    coordinator.start();
    require(coordinator.running(), "start must return before asynchronous work completes");
    require(
        waitUntil([controlled]() { return controlled->request_count == 1; }),
        "the first provider request must be scheduled"
    );
    require(controlled->requested.photo_count == 2, "shared coordinates must retain photo count");
    controlled->succeed();
    require(
        waitUntil([&place_changes]() { return place_changes == 1; }),
        "one recorded result must publish one place change"
    );
    require(persisted.latitude_e7 == 312'304'000, "Catalog latitude must come from the candidate");
    require(
        persisted.longitude_e7 == 1'212'473'000,
        "Catalog longitude must come from the candidate"
    );
    require(persisted.locality == QStringLiteral("Shanghai"), "locality must survive projection");
    require(coordinator.recordedCount() == 1, "recorded count must advance exactly once");
    require(coordinator.processedCount() == 1, "processed count must advance exactly once");
    require(coordinator.failedCount() == 0, "successful pass recorded a failure");
    require(
        waitUntil([&coordinator]() { return !coordinator.running(); }),
        "an empty follow-up queue must stop the pass"
    );
    require(
        coordinator.statusCode() == QStringLiteral("complete"),
        "successful pass did not publish completion"
    );
}

void unavailableProviderIsAnIntentionalNoOp() {
    auto provider = std::make_unique<ControlledReverseGeocoder>();
    auto* const controlled = provider.get();
    controlled->available_ = false;
    bool unexpected_operation = false;
    ReviewLibraryPlaceResolutionCoordinator coordinator(
        {
            .candidates =
                [&unexpected_operation](const std::uint32_t) {
                    unexpected_operation = true;
                    return QVector<BackendLibraryPlaceResolutionCandidate>{};
                },
            .record =
                [&unexpected_operation](const BackendLibraryPlaceResolutionResult&) {
                    unexpected_operation = true;
                    return BackendRecordLibraryPlaceResolutionStatus::CoordinatesNoLongerUsed;
                },
        },
        std::move(provider)
    );

    coordinator.start();
    QCoreApplication::processEvents();
    require(!coordinator.running(), "unavailable provider must stay idle");
    require(controlled->request_count == 0, "unavailable provider must not receive a request");
    require(!unexpected_operation, "unavailable provider must not access the Catalog");
    require(
        coordinator.statusCode() == QStringLiteral("permission-required"),
        "unavailable provider did not publish its permission state"
    );
}

void failedCoordinatesRemainRetryableAfterAnExplicitRestart() {
    auto provider = std::make_unique<ControlledReverseGeocoder>();
    auto* const controlled = provider.get();
    bool recorded = false;
    ReviewLibraryPlaceResolutionCoordinator coordinator(
        {
            .candidates =
                [&recorded](const std::uint32_t) {
                    if (recorded) {
                        return QVector<BackendLibraryPlaceResolutionCandidate>{};
                    }
                    return QVector<BackendLibraryPlaceResolutionCandidate>{
                        {
                            .latitude_e7 = 446'198'000,
                            .longitude_e7 = -1'104'255'267,
                            .photo_count = 1,
                        },
                    };
                },
            .record =
                [&recorded](const BackendLibraryPlaceResolutionResult&) {
                    recorded = true;
                    return BackendRecordLibraryPlaceResolutionStatus::Recorded;
                },
        },
        std::move(provider)
    );

    coordinator.start();
    require(
        waitUntil([controlled]() { return controlled->request_count == 1; }),
        "initial provider request was not scheduled"
    );
    controlled->fail(QStringLiteral("temporary provider failure"));
    require(
        waitUntil([&coordinator]() { return !coordinator.running(); }),
        "failed pass did not settle"
    );
    require(
        coordinator.statusCode() == QStringLiteral("failed") && coordinator.processedCount() == 1
            && coordinator.failedCount() == 1
            && coordinator.errorText() == QStringLiteral("temporary provider failure"),
        "failed pass state was not preserved"
    );

    coordinator.start();
    require(
        waitUntil([controlled]() { return controlled->request_count == 2; }),
        "explicit retry did not request the failed coordinates again"
    );
    controlled->succeed();
    require(
        waitUntil([&coordinator]() { return !coordinator.running(); })
            && coordinator.statusCode() == QStringLiteral("complete")
            && coordinator.recordedCount() == 1 && coordinator.failedCount() == 0,
        "retried coordinate did not complete successfully"
    );
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    resolvesAfterExplicitStartWithoutBlockingTheCaller();
    unavailableProviderIsAnIntentionalNoOp();
    failedCoordinatesRemainRetryableAfterAnExplicitRestart();
    return EXIT_SUCCESS;
}

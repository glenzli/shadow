#include "review_focus_detail_coordinator.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "review focus detail coordinator contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Predicate> void wait_until(Predicate predicate, const std::string& message) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 3'000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), message);
}

void current_request_publishes_one_bounded_image() {
    auto store = std::make_shared<ReviewFocusDetailStore>();
    std::atomic_uint64_t token{0};
    ReviewFocusDetailCoordinator coordinator(
        [](const ReviewFocusDetailRequest& request) {
            require(request.center_x == 0.25, "focus x was not retained");
            require(request.center_y == 0.75, "focus y was not retained");
            QImage image(384, 384, QImage::Format_RGB888);
            image.fill(Qt::red);
            return image;
        },
        [&token]() { return ++token; },
        store
    );

    coordinator.request(QStringLiteral("photo-a"), QStringLiteral("/photo-a.nef"), 0.25, 0.75);
    wait_until([&coordinator]() { return !coordinator.busy(); }, "request did not finish");
    require(coordinator.ready(), "current result was not ready");
    require(!coordinator.failed(), "current result failed");
    require(
        store->snapshot(1).pixelColor(0, 0) == QColor(Qt::red),
        "published image did not retain the current generation"
    );
}

void rapid_navigation_coalesces_to_newest_identity() {
    auto store = std::make_shared<ReviewFocusDetailStore>();
    std::atomic_uint64_t token{0};
    QSemaphore first_started;
    QSemaphore release_first;
    ReviewFocusDetailCoordinator coordinator(
        [&first_started, &release_first](const ReviewFocusDetailRequest& request) {
            if (request.photo_id == QStringLiteral("photo-a")) {
                first_started.release();
                release_first.acquire();
            }
            QImage image(32, 32, QImage::Format_RGB888);
            image.fill(request.photo_id == QStringLiteral("photo-b") ? Qt::green : Qt::red);
            return image;
        },
        [&token]() { return ++token; },
        store
    );

    coordinator.request(QStringLiteral("photo-a"), QStringLiteral("/photo-a.nef"), 0.5, 0.5);
    wait_until(
        [&first_started]() { return first_started.available() > 0; },
        "first request did not start"
    );
    first_started.acquire();
    coordinator.request(QStringLiteral("photo-b"), QStringLiteral("/photo-b.nef"), 0.4, 0.6);
    release_first.release();
    wait_until([&coordinator]() { return !coordinator.busy(); }, "newest request did not finish");

    require(coordinator.ready(), "newest result was not ready");
    require(store->snapshot(1).isNull(), "stale generation remained visible");
    require(
        store->snapshot(2).pixelColor(0, 0) == QColor(Qt::green),
        "newest selection was not published"
    );
    require(token.load() == 2, "navigation did not invalidate the prior render token");
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    current_request_publishes_one_bounded_image();
    rapid_navigation_coalesces_to_newest_identity();
    return EXIT_SUCCESS;
}

#include "bulk_import.hpp"

#include "../review_controller.hpp"

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QVector>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>

namespace DesktopSmoke {
namespace {

struct ImportObservation {
    QElapsedTimer clock;
    qint64 previous_tick = 0;
    qint64 first_rows_ms = -1;
    qint64 first_thumbnail_ms = -1;
    qint64 first_embedded_ms = -1;
    qint64 first_generated_ms = -1;
    qint64 last_wheel_ms = -1;
    qint64 pending_wheel_ms = -1;
    qint64 maximum_scroll_response_ms = 0;
    qreal previous_scroll_y = 0;
    qreal maximum_scroll_y = 0;
    qreal minimum_remaining_scroll = std::numeric_limits<qreal>::max();
    bool paged_during_import = false;
    bool scrolled_during_import = false;
    int maximum_rows_during_import = 0;
    std::array<quint64, 9> gaps{};
    quint64 gap_samples = 0;
    qint64 maximum_gap_ms = 0;
};

constexpr std::array<qint64, 8> gap_limits{20, 32, 64, 128, 256, 512, 1024, 2048};

qint64 p95_gap(const ImportObservation& state) {
    const auto rank = (state.gap_samples * 95 + 99) / 100;
    quint64 count = 0;
    for (std::size_t index = 0; index < state.gaps.size(); ++index) {
        count += state.gaps[index];
        if (count >= rank) {
            return index < gap_limits.size() ? gap_limits[index] : state.maximum_gap_ms;
        }
    }
    return 0;
}

void observe_thumbnails(QQuickItem* root, ImportObservation& state, const qint64 elapsed) {
    if (root == nullptr || (state.first_embedded_ms >= 0 && state.first_generated_ms >= 0)) {
        return;
    }
    // ListView owns delegates in the visual tree; QObject ancestry does not include them.
    QVector<QQuickItem*> pending{root};
    while (!pending.isEmpty()) {
        auto* const item = pending.takeLast();
        pending.append(item->childItems());
        if (item->objectName() != QStringLiteral("reviewPhotoThumbnail") || !item->isVisible()
            || item->property("status").toInt() != 1) {
            continue;
        }
        if (state.first_thumbnail_ms < 0) {
            state.first_thumbnail_ms = elapsed;
        }
        auto* card = item->parentItem();
        while (card != nullptr && !card->property("visualRole").isValid()) {
            card = card->parentItem();
        }
        const auto role = card != nullptr ? card->property("visualRole").toString() : QString{};
        if (role == QStringLiteral("embedded") && state.first_embedded_ms < 0) {
            state.first_embedded_ms = elapsed;
            qInfo() << "Bulk import first embedded preview ready ms" << elapsed;
        }
        if (role == QStringLiteral("proxy") && state.first_generated_ms < 0) {
            state.first_generated_ms = elapsed;
            qInfo() << "Bulk import first generated preview ready ms" << elapsed;
        }
    }
}

} // namespace

void startBulkImportLifecycle(
    QApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& controller
) {
    auto state = std::make_shared<ImportObservation>();
    state->clock.start();
    auto* const timer = new QTimer(&application);
    timer->setInterval(20);
    timer->setTimerType(Qt::PreciseTimer);
    QObject::connect(
        &controller,
        &ReviewController::loadingMoreChanged,
        timer,
        [state, &controller]() {
            if (controller.scanning() && controller.loadingMore()) {
                state->paged_during_import = true;
                qInfo() << "Bulk import page admitted while scanning rows"
                        << controller.reviewModel()->rowCount();
            }
        }
    );
    QObject::connect(
        timer,
        &QTimer::timeout,
        timer,
        [state, timer, &application, &engine, &controller]() {
            if (engine.rootObjects().isEmpty()) {
                return;
            }
            QObject* const root = engine.rootObjects().front();
            auto* const window = qobject_cast<QQuickWindow*>(root);
            auto* const grid = root->findChild<QQuickItem*>(QStringLiteral("reviewJustifiedGrid"));
            const auto elapsed = state->clock.elapsed();
            const auto gap = elapsed - state->previous_tick;
            state->previous_tick = elapsed;
            std::size_t bucket = 0;
            while (bucket < gap_limits.size() && gap > gap_limits[bucket]) {
                ++bucket;
            }
            ++state->gaps[bucket];
            ++state->gap_samples;
            state->maximum_gap_ms = std::max(state->maximum_gap_ms, gap);
            const int rows = controller.reviewModel()->rowCount();
            if (rows > 0 && state->first_rows_ms < 0) {
                state->first_rows_ms = elapsed;
            }
            if (controller.scanning()) {
                state->maximum_rows_during_import =
                    std::max(state->maximum_rows_during_import, rows);
            }
            observe_thumbnails(
                window != nullptr ? window->contentItem() : nullptr,
                *state,
                elapsed
            );
            if (grid != nullptr) {
                const auto y = grid->property("contentY").toReal();
                if (controller.scanning() && rows > 16) {
                    state->maximum_scroll_y = std::max(state->maximum_scroll_y, y);
                    state->minimum_remaining_scroll = std::min(
                        state->minimum_remaining_scroll,
                        std::max<qreal>(
                            0,
                            grid->property("contentHeight").toReal() - grid->height() - y
                        )
                    );
                }
                if (state->pending_wheel_ms >= 0 && std::abs(y - state->previous_scroll_y) > 1) {
                    state->maximum_scroll_response_ms = std::max(
                        state->maximum_scroll_response_ms,
                        elapsed - state->pending_wheel_ms
                    );
                    state->scrolled_during_import |= controller.scanning();
                    state->pending_wheel_ms = -1;
                }
                if (state->pending_wheel_ms >= 0 && elapsed - state->pending_wheel_ms > 1'000) {
                    state->pending_wheel_ms = -1;
                }
                if (window != nullptr && controller.scanning() && rows > 16
                    && state->pending_wheel_ms < 0 && elapsed - state->last_wheel_ms > 300) {
                    const auto position =
                        grid->mapToScene(QPointF(grid->width() / 2, grid->height() / 2));
                    state->previous_scroll_y = y;
                    state->pending_wheel_ms = elapsed;
                    state->last_wheel_ms = elapsed;
                    QCoreApplication::postEvent(
                        window,
                        new QWheelEvent(
                            position,
                            position,
                            QPoint(),
                            QPoint(0, -720),
                            Qt::NoButton,
                            Qt::NoModifier,
                            Qt::NoScrollPhase,
                            false
                        )
                    );
                }
            }
            const auto expected =
                qEnvironmentVariableIntValue("SHADOW_DESKTOP_BULK_EXPECTED_PHOTOS");
            if (controller.scanning() || controller.refreshing() || controller.loadingMore()
                || controller.itemCount() < std::max(1, expected)) {
                return;
            }
            const auto progress = controller.scanProgress();
            const bool success =
                progress.value(QStringLiteral("phase")).toString() == QStringLiteral("completed")
                && state->first_embedded_ms >= 0 && state->first_generated_ms >= 0
                && state->paged_during_import && state->scrolled_during_import
                && state->maximum_rows_during_import > 96
                && progress.value(QStringLiteral("decodeHardFailures")).toULongLong() == 0
                && progress.value(QStringLiteral("previewFailures")).toULongLong() == 0;
            const QJsonObject report{
                {QStringLiteral("success"), success},
                {QStringLiteral("first_rows_ms"), state->first_rows_ms},
                {QStringLiteral("first_thumbnail_ready_ms"), state->first_thumbnail_ms},
                {QStringLiteral("first_embedded_ready_ms"), state->first_embedded_ms},
                {QStringLiteral("first_generated_ready_ms"), state->first_generated_ms},
                {QStringLiteral("paged_during_import"), state->paged_during_import},
                {QStringLiteral("scrolled_during_import"), state->scrolled_during_import},
                {QStringLiteral("maximum_rows_during_import"), state->maximum_rows_during_import},
                {QStringLiteral("maximum_scroll_y"), state->maximum_scroll_y},
                {QStringLiteral("minimum_remaining_scroll"), state->minimum_remaining_scroll},
                {QStringLiteral("event_loop_gap_p95_upper_ms"), p95_gap(*state)},
                {QStringLiteral("maximum_observed_scroll_response_ms"),
                 state->maximum_scroll_response_ms},
                {QStringLiteral("complete_ms"), elapsed},
                {QStringLiteral("total_photos"), controller.itemCount()},
            };
            qInfo().noquote() << "Bulk import acceptance"
                              << QJsonDocument(report).toJson(QJsonDocument::Compact);
            timer->stop();
            application.exit(success ? EXIT_SUCCESS : EXIT_FAILURE);
        }
    );
    timer->start();
}

} // namespace DesktopSmoke

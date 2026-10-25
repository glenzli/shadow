#include "bulk_import.hpp"

#include "../review_controller.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
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
    bool fixed_workload = qEnvironmentVariableIsSet("SHADOW_DESKTOP_BULK_FIXED_WORKLOAD");
    bool fixed_window = false;
    int pages_requested = 0;
    int step = 0;
    qint64 step_started_ms = -1;
    qint64 last_position_ms = -1;
    QString target_photo;
    QString target_representation;
    QString target_title;
    QString target_source;
    QJsonArray steps;
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
constexpr std::array<int, 4> fixed_target_rows{288, 192, 96, 0};

QQuickItem* visible_grid(QQuickItem* root) {
    if (!root || !root->isVisible())
        return nullptr;
    if (root->objectName() == QStringLiteral("reviewJustifiedGrid"))
        return root;
    for (auto* child : root->childItems())
        if (auto* found = visible_grid(child))
            return found;
    return nullptr;
}

void disable_automatic_paging(QQuickItem* root) {
    if (!root)
        return;
    if (root->objectName() == QStringLiteral("reviewJustifiedGrid"))
        root->setProperty("automaticPagingEnabled", false);
    for (auto* child : root->childItems())
        disable_automatic_paging(child);
}

bool target_ready(QQuickItem* root, const ImportObservation& state) {
    if (!root)
        return false;
    QVector<QQuickItem*> pending{root};
    while (!pending.isEmpty()) {
        auto* item = pending.takeLast();
        pending.append(item->childItems());
        if (item->objectName() != QStringLiteral("reviewPhotoThumbnail") || !item->isVisible()
            || item->property("status").toInt() != 1)
            continue;
        auto* card = item->parentItem();
        while (card && !card->property("visualRole").isValid())
            card = card->parentItem();
        if (card && card->property("photoId").toString() == state.target_photo
            && card->property("representationId").toString() == state.target_representation
            && card->property("visualRole").toString() == QStringLiteral("proxy")) {
            const auto rect = item->mapRectToItem(root, item->boundingRect());
            if (root->boundingRect().intersects(rect))
                return true;
        }
    }
    return false;
}

// Four pages, then the same four photo positions regardless of import duration.
// A step completes only after its actual QML image has the developed proxy.
void advance_fixed_workload(
    ReviewController& controller,
    QQuickItem* grid,
    ImportObservation& state,
    qint64 elapsed
) {
    grid->setProperty("automaticPagingEnabled", false);
    if (controller.refreshing() || controller.loadingMore() || controller.busy())
        return;
    auto* model = controller.reviewModel();
    // An append can be queued behind a streaming refresh without loadingMore
    // becoming true yet. Wait for its rows before requesting the next page.
    if (model->rowCount() < 96 * (state.pages_requested + 1))
        return;
    if (state.pages_requested < 3) {
        if (controller.hasMore()) {
            qInfo() << "Fixed bulk workload requests page" << state.pages_requested + 2;
            controller.loadMore();
            ++state.pages_requested;
        }
        return;
    }
    if (model->rowCount() != 384 || state.step == static_cast<int>(fixed_target_rows.size()))
        return;
    if (state.step_started_ms < 0) {
        const auto index = model->index(fixed_target_rows[static_cast<std::size_t>(state.step)], 0);
        state.target_photo = model->data(index, ReviewModel::PhotoIdRole).toString();
        state.target_representation =
            model->data(index, ReviewModel::RepresentationIdRole).toString();
        state.target_title = model->data(index, ReviewModel::TitleRole).toString();
        state.target_source =
            QDir(qEnvironmentVariable("SHADOW_DESKTOP_SCAN_FOLDER"))
                .relativeFilePath(model->data(index, ReviewModel::SourcePathRole).toString());
        QVariant positioned;
        QMetaObject::invokeMethod(
            grid,
            "positionAtPhoto",
            Q_RETURN_ARG(QVariant, positioned),
            Q_ARG(QVariant, state.target_photo),
            Q_ARG(QVariant, state.target_representation)
        );
        if (!positioned.toBool())
            return;
        state.step_started_ms = elapsed;
        state.last_position_ms = elapsed;
        qInfo() << "Fixed bulk workload target row"
                << fixed_target_rows[static_cast<std::size_t>(state.step)] << "at ms" << elapsed;
        state.scrolled_during_import |= controller.scanning();
        return;
    }
    if (elapsed - state.step_started_ms >= 100 && target_ready(grid, state)) {
        qInfo() << "Fixed bulk workload proxy ready after ms" << elapsed - state.step_started_ms;
        state.steps.append(
            QJsonObject{
                {QStringLiteral("source_row"),
                 fixed_target_rows[static_cast<std::size_t>(state.step)]},
                {QStringLiteral("title"), state.target_title},
                {QStringLiteral("source"), state.target_source},
                {QStringLiteral("proxy_ready_ms"), elapsed - state.step_started_ms},
            }
        );
        ++state.step;
        state.step_started_ms = -1;
    } else if (elapsed - state.last_position_ms >= 100) {
        // Cached dimensions can settle after the first accepted position call.
        // Keep the same target through ListView's asynchronous layout/anchor
        // updates; readiness still requires its real, visible QML image.
        QMetaObject::invokeMethod(
            grid,
            "positionAtPhoto",
            Q_ARG(QVariant, state.target_photo),
            Q_ARG(QVariant, state.target_representation)
        );
        state.last_position_ms = elapsed;
    }
}

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
            || item->property("status").toInt() != 1
            || !root->boundingRect().intersects(item->mapRectToItem(root, item->boundingRect()))) {
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
    if (state->fixed_workload) {
        for (auto* root : engine.rootObjects()) {
            if (auto* window = qobject_cast<QQuickWindow*>(root)) {
                // Disable at installation, before resize/layout and the first
                // event-loop turn can admit an automatic page.
                disable_automatic_paging(window->contentItem());
                window->resize(1440, 900);
            }
        }
        state->fixed_window = true;
    }
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
            if (window && state->fixed_workload && !state->fixed_window) {
                disable_automatic_paging(window->contentItem());
                window->resize(1440, 900);
                state->fixed_window = true;
            }
            auto* const grid = visible_grid(window ? window->contentItem() : nullptr);
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
            if (state->fixed_workload && rows > 384) {
                qCritical() << "Fixed bulk workload exceeded four pages" << rows;
                timer->stop();
                application.exit(EXIT_FAILURE);
                return;
            }
            if (rows > 0 && state->first_rows_ms < 0) {
                state->first_rows_ms = elapsed;
            }
            if (controller.scanning()) {
                state->maximum_rows_during_import =
                    std::max(state->maximum_rows_during_import, rows);
            }
            observe_thumbnails(
                state->fixed_workload ? grid
                : window != nullptr   ? window->contentItem()
                                      : nullptr,
                *state,
                elapsed
            );
            if (grid != nullptr && state->fixed_workload) {
                advance_fixed_workload(controller, grid, *state, elapsed);
            }
            if (grid != nullptr && !state->fixed_workload) {
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
                || controller.itemCount() < std::max(1, expected)
                || (state->fixed_workload && state->step < 4)) {
                return;
            }
            const auto progress = controller.scanProgress();
            const bool success =
                progress.value(QStringLiteral("phase")).toString() == QStringLiteral("completed")
                && (state->fixed_workload || state->first_embedded_ms >= 0)
                && state->first_generated_ms >= 0 && state->paged_during_import
                && state->scrolled_during_import && state->maximum_rows_during_import > 96
                && progress.value(QStringLiteral("decodeHardFailures")).toULongLong() == 0
                && progress.value(QStringLiteral("previewFailures")).toULongLong() == 0;
            const QJsonObject report{
                {QStringLiteral("workload"),
                 state->fixed_workload ? QStringLiteral("four-pages-four-targets-v1")
                                       : QStringLiteral("continuous-wheel-v1")},
                {QStringLiteral("pages_requested"), state->pages_requested},
                {QStringLiteral("loaded_rows"), rows},
                {QStringLiteral("steps"), state->steps},
                {QStringLiteral("window_size"),
                 QJsonArray{window ? window->width() : 0, window ? window->height() : 0}},
                {QStringLiteral("device_pixel_ratio"), window ? window->devicePixelRatio() : 0},
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

#include "desktop_smoke/edit_preview_session.hpp"

#include "edit_controller.hpp"
#include "edit_preview_provider.hpp"
#include "review_controller.hpp"

#include <QAbstractItemModel>
#include <QColorSpace>
#include <QCoreApplication>
#include <QDebug>
#include <QImage>
#include <QModelIndex>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>

namespace {

constexpr int CURRENT_PREVIEW_TIMEOUT_MS = 30'000;
constexpr int FULL_DETAIL_TIMEOUT_MS = 120'000;
constexpr int TERMINAL_SETTLE_MS = 50;
constexpr int FRAME_POLL_MS = 20;
constexpr std::uint32_t DETAIL_VIEWPORT_WIDTH = 1'280;
constexpr std::uint32_t DETAIL_VIEWPORT_HEIGHT = 960;
constexpr std::uint32_t DETAIL_TILE_SIDE = 512;
constexpr double DETAIL_CENTER = 0.5;

[[nodiscard]] QString imageProviderRequestId(const QString& source) {
    const QUrl url(source);
    QString id = url.path();
    if (id.startsWith(QLatin1Char('/'))) {
        id.remove(0, 1);
    }
    const QString query = url.query(QUrl::FullyEncoded);
    if (!query.isEmpty()) {
        id += QLatin1Char('?');
        id += query;
    }
    return id;
}

[[nodiscard]] QString previewGeneration(const QString& source) {
    return QUrlQuery(QUrl(source)).queryItemValue(QStringLiteral("generation"));
}

[[nodiscard]] bool
validEditHistogram(const QVariantMap& histogram, const QString& source) {
    const QString source_generation = previewGeneration(source);
    if (!histogram.value(QStringLiteral("valid")).toBool()
        || histogram.value(QStringLiteral("updating")).toBool()
        || histogram.value(QStringLiteral("stale")).toBool()
        || histogram.value(QStringLiteral("version")).toString().isEmpty()
        || source_generation.isEmpty()
        || histogram.value(QStringLiteral("generation")).toString() != source_generation
        || histogram.value(QStringLiteral("targetGeneration")).toString()
               != source_generation) {
        return false;
    }
    const qulonglong pixel_count =
        histogram.value(QStringLiteral("pixelCount")).toULongLong();
    const qulonglong dimensions_count =
        histogram.value(QStringLiteral("width")).toULongLong()
        * histogram.value(QStringLiteral("height")).toULongLong();
    if (pixel_count == 0 || pixel_count != dimensions_count) {
        return false;
    }
    for (const auto& key : {
             QStringLiteral("red"),
             QStringLiteral("green"),
             QStringLiteral("blue"),
             QStringLiteral("luma"),
         }) {
        const QVariantList bins = histogram.value(key).toList();
        if (bins.size() != 256) {
            return false;
        }
        qulonglong sum = 0;
        for (const QVariant& bin : bins) {
            const qulonglong count = bin.toULongLong();
            if (count > pixel_count - sum) {
                return false;
            }
            sum += count;
        }
        if (sum != pixel_count) {
            return false;
        }
    }
    return histogram.value(QStringLiteral("belowZero")).toList().size() == 3
           && histogram.value(QStringLiteral("aboveOne")).toList().size() == 3
           && histogram.value(QStringLiteral("shadowClippedPixels")).toULongLong()
                  <= pixel_count
           && histogram.value(QStringLiteral("highlightClippedPixels")).toULongLong()
                  <= pixel_count;
}

[[nodiscard]] QObject* precisionWorkspace(QQmlApplicationEngine& engine) {
    if (engine.rootObjects().isEmpty()) {
        return nullptr;
    }
    return engine.rootObjects().front()->findChild<QObject*>(
        QStringLiteral("precisionWorkspace")
    );
}

[[nodiscard]] bool
qmlPreviewIsReady(QQmlApplicationEngine& engine, const QString& source) {
    const auto* const workspace = precisionWorkspace(engine);
    if (workspace == nullptr || previewGeneration(source).isEmpty()) {
        return false;
    }
    if (QUrl(source).path().endsWith(QStringLiteral("/before"))) {
        return workspace->property("beforeFrameReady").toBool();
    }
    return workspace->property("readyPreviewGeneration").toString()
           == previewGeneration(source);
}

struct DetailAxisGrid final {
    std::uint32_t start = 0;
    std::uint32_t extent = 0;
};

[[nodiscard]] DetailAxisGrid
expectedDetailAxisGrid(const std::uint32_t full, const std::uint32_t requested) {
    const std::uint32_t span = std::min(full, requested);
    const std::uint32_t max_start = full - span;
    const double centered =
        DETAIL_CENTER * static_cast<double>(full) - static_cast<double>(span) / 2.0;
    const double rounded_start =
        std::clamp(std::round(centered), 0.0, static_cast<double>(max_start));
    const auto viewport_start = static_cast<std::uint32_t>(rounded_start);
    const std::uint32_t grid_start =
        viewport_start / DETAIL_TILE_SIDE * DETAIL_TILE_SIDE;
    const std::uint64_t viewport_end =
        static_cast<std::uint64_t>(viewport_start) + span;
    const std::uint64_t aligned_end =
        (viewport_end + DETAIL_TILE_SIDE - 1U) / DETAIL_TILE_SIDE * DETAIL_TILE_SIDE;
    const auto grid_end =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(full, aligned_end));
    return {.start = grid_start, .extent = grid_end - grid_start};
}

class EditPreviewSession final
    : public std::enable_shared_from_this<EditPreviewSession> {
public:
    static void start(
        QCoreApplication& application,
        QQmlApplicationEngine& engine,
        ReviewController& review,
        EditController& editor,
        EditPreviewProvider* const edit_preview_provider,
        DesktopSmoke::EditPreviewSessionOptions options
    ) {
        const auto session = std::shared_ptr<EditPreviewSession>(new EditPreviewSession(
            application, engine, review, editor, edit_preview_provider, options
        ));
        session->connectSignals();
    }

private:
    enum class Stage : std::uint8_t {
        AwaitReviewItem,
        AwaitCurrentPreview,
        AwaitBeforePreview,
        AwaitFullDetail,
        Finished,
        Failed,
    };

    EditPreviewSession(
        QCoreApplication& application,
        QQmlApplicationEngine& engine,
        ReviewController& review,
        EditController& editor,
        EditPreviewProvider* const edit_preview_provider,
        DesktopSmoke::EditPreviewSessionOptions options
    )
        : application_(application),
          engine_(engine),
          review_(review),
          editor_(editor),
          edit_preview_provider_(edit_preview_provider),
          options_(options) {}

    void connectSignals() {
        if (engine_.rootObjects().isEmpty()) {
            fail(QStringLiteral("the application has no loaded QML root"));
            return;
        }
        if (options_.request_full_detail && edit_preview_provider_ == nullptr) {
            fail(QStringLiteral("the full-detail image provider is unavailable"));
            return;
        }

        const auto self = shared_from_this();
        const auto evaluate = [self]() { self->evaluate(); };
        QObject::connect(
            &review_, &ReviewController::itemCountChanged, &application_, evaluate
        );
        QObject::connect(
            review_.reviewModel(),
            &QAbstractItemModel::modelReset,
            &application_,
            evaluate
        );
        QObject::connect(
            review_.reviewModel(),
            &QAbstractItemModel::rowsInserted,
            &application_,
            [self](const QModelIndex&, int, int) { self->evaluate(); }
        );
        QObject::connect(
            &editor_, &EditController::activeChanged, &application_, evaluate
        );
        QObject::connect(
            &editor_, &EditController::stateBusyChanged, &application_, evaluate
        );
        QObject::connect(
            &editor_, &EditController::previewSourceChanged, &application_, evaluate
        );
        QObject::connect(
            &editor_, &EditController::histogramChanged, &application_, evaluate
        );
        QObject::connect(
            &editor_,
            &EditController::beforePreviewSourceChanged,
            &application_,
            evaluate
        );
        QObject::connect(
            &editor_, &EditController::beforeHistogramChanged, &application_, evaluate
        );
        QObject::connect(
            &editor_, &EditController::detailRenderingChanged, &application_, evaluate
        );
        QObject::connect(
            &editor_, &EditController::detailGeometryChanged, &application_, evaluate
        );
        QObject::connect(
            &editor_, &EditController::detailTilesChanged, &application_, evaluate
        );
        QObject::connect(
            &editor_, &EditController::detailErrorTextChanged, &application_, evaluate
        );
        QObject::connect(
            &editor_,
            &EditController::fullResolutionStateChanged,
            &application_,
            evaluate
        );

        frame_poll_.setInterval(FRAME_POLL_MS);
        QObject::connect(&frame_poll_, &QTimer::timeout, &application_, evaluate);
        frame_poll_.start();

        deadline_.setSingleShot(true);
        deadline_.setInterval(
            options_.request_full_detail ? FULL_DETAIL_TIMEOUT_MS
                                         : CURRENT_PREVIEW_TIMEOUT_MS
        );
        QObject::connect(&deadline_, &QTimer::timeout, &application_, [self]() {
            self->fail(QStringLiteral("timed out before the acceptance terminal"));
        });
        deadline_.start();
        QTimer::singleShot(0, &application_, evaluate);
    }

    void evaluate() {
        switch (stage_) {
        case Stage::AwaitReviewItem:
            openFirstPhotoIfReady();
            break;
        case Stage::AwaitCurrentPreview:
            acceptCurrentPreviewIfReady();
            break;
        case Stage::AwaitBeforePreview:
            acceptBeforePreviewIfReady();
            break;
        case Stage::AwaitFullDetail:
            acceptFullDetailIfReady();
            break;
        case Stage::Finished:
        case Stage::Failed:
            break;
        }
    }

    void openFirstPhotoIfReady() {
        auto* const model = review_.reviewModel();
        if (model == nullptr || model->rowCount() == 0) {
            return;
        }
        const QModelIndex first = model->index(0, 0);
        const QString photo_id =
            model->data(first, ReviewModel::PhotoIdRole).toString();
        const QString representation_id =
            model->data(first, ReviewModel::RepresentationIdRole).toString();
        const QString source_path =
            model->data(first, ReviewModel::SourcePathRole).toString();
        const QString title = model->data(first, ReviewModel::TitleRole).toString();
        if (photo_id.isEmpty() || representation_id.isEmpty()
            || source_path.isEmpty()) {
            fail(
                QStringLiteral("the first Review row has an incomplete source identity")
            );
            return;
        }

        opened_photo_id_ = photo_id;
        opened_representation_id_ = representation_id;
        stage_ = Stage::AwaitCurrentPreview;
        editor_.openPhoto(photo_id, representation_id, source_path, title);
        engine_.rootObjects().front()->setProperty("workspaceIndex", 1);
        evaluate();
    }

    void acceptCurrentPreviewIfReady() {
        const QString source = editor_.previewSource();
        if (!editor_.active() || source.isEmpty()
            || !validEditHistogram(editor_.histogram(), source)
            || !qmlPreviewIsReady(engine_, source)) {
            return;
        }
        current_source_ = source;
        if (options_.request_before) {
            stage_ = Stage::AwaitBeforePreview;
            editor_.requestBeforePreview();
            evaluate();
        } else if (options_.request_full_detail) {
            requestFullDetail();
        } else {
            succeed();
        }
    }

    void acceptBeforePreviewIfReady() {
        const QString source = editor_.beforePreviewSource();
        if (source.isEmpty() || !validEditHistogram(editor_.beforeHistogram(), source)
            || !qmlPreviewIsReady(engine_, source)) {
            return;
        }
        before_source_ = source;
        if (options_.request_full_detail) {
            requestFullDetail();
        } else {
            succeed();
        }
    }

    void requestFullDetail() {
        stage_ = Stage::AwaitFullDetail;
        editor_.requestDetailViewport(
            DETAIL_CENTER,
            DETAIL_CENTER,
            static_cast<int>(DETAIL_VIEWPORT_WIDTH),
            static_cast<int>(DETAIL_VIEWPORT_HEIGHT)
        );
        evaluate();
    }

    void acceptFullDetailIfReady() {
        if (!editor_.detailErrorText().isEmpty()) {
            fail(QStringLiteral("the full-detail renderer failed: %1")
                     .arg(editor_.detailErrorText()));
            return;
        }
        const QVariantList tiles = editor_.detailTiles();
        if (tiles.isEmpty() || editor_.detailRendering()
            || editor_.fullResolutionPreparing() || !editor_.fullResolutionReady()) {
            return;
        }
        if (tiles.size() != 1 || !editor_.detailMode() || editor_.detailFullWidth() == 0
            || editor_.detailFullHeight() == 0 || editor_.detailRetainedBytes() == 0
            || editor_.fullResolutionRetainedBytes() != editor_.detailRetainedBytes()) {
            fail(QStringLiteral(
                "the detail publication is not one settled atomic viewport"
            ));
            return;
        }

        const QVariantMap viewport = tiles.front().toMap();
        const std::uint32_t x = viewport.value(QStringLiteral("x")).toUInt();
        const std::uint32_t y = viewport.value(QStringLiteral("y")).toUInt();
        const std::uint32_t width = viewport.value(QStringLiteral("width")).toUInt();
        const std::uint32_t height = viewport.value(QStringLiteral("height")).toUInt();
        const QString source = viewport.value(QStringLiteral("source")).toString();
        const DetailAxisGrid expected_x =
            expectedDetailAxisGrid(editor_.detailFullWidth(), DETAIL_VIEWPORT_WIDTH);
        const DetailAxisGrid expected_y =
            expectedDetailAxisGrid(editor_.detailFullHeight(), DETAIL_VIEWPORT_HEIGHT);
        if (x != expected_x.start || y != expected_y.start || width != expected_x.extent
            || height != expected_y.extent || source.isEmpty()) {
            fail(QStringLiteral(
                "the atomic detail viewport does not cover the "
                "complete requested grid"
            ));
            return;
        }

        const QUrl source_url(source);
        const QUrlQuery source_query(source_url);
        bool photo_ok = false;
        bool recipe_ok = false;
        bool viewport_ok = false;
        const qulonglong photo_generation =
            source_query.queryItemValue(QStringLiteral("photo")).toULongLong(&photo_ok);
        const qulonglong recipe_generation =
            source_query.queryItemValue(QStringLiteral("recipe"))
                .toULongLong(&recipe_ok);
        const qulonglong viewport_generation =
            source_query.queryItemValue(QStringLiteral("viewport"))
                .toULongLong(&viewport_ok);
        const QString expected_ticket =
            QStringLiteral("/detail/viewport-%1-%2").arg(x).arg(y);
        if (source_url.scheme() != QStringLiteral("image")
            || source_url.host() != QStringLiteral("shadow-edit")
            || source_url.path() != expected_ticket || !photo_ok || !recipe_ok
            || !viewport_ok || photo_generation == 0 || recipe_generation == 0
            || viewport_generation == 0) {
            fail(QStringLiteral(
                "the detail image URL lacks its exact generation contract"
            ));
            return;
        }

        QSize decoded_size;
        const QImage image = edit_preview_provider_->requestImage(
            imageProviderRequestId(source), &decoded_size, {}
        );
        const QSize expected_size(static_cast<int>(width), static_cast<int>(height));
        if (image.isNull() || decoded_size != expected_size
            || image.size() != expected_size
            || image.colorSpace() != QColorSpace(QColorSpace::SRgb)
            || image.format() != QImage::Format_RGB888) {
            fail(QStringLiteral(
                "the complete detail viewport could not be read back "
                "as display-sRGB RGB8"
            ));
            return;
        }
        detail_source_ = source;
        succeed();
    }

    void succeed() {
        if (stage_ == Stage::Finished || stage_ == Stage::Failed) {
            return;
        }
        stage_ = Stage::Finished;
        frame_poll_.stop();
        deadline_.stop();
        qInfo().noquote() << "Edit preview smoke passed:"
                          << "photo" << opened_photo_id_ << "representation"
                          << opened_representation_id_ << "current" << current_source_
                          << "before"
                          << (options_.request_before
                                  ? before_source_
                                  : QStringLiteral("<not requested>"))
                          << "detail"
                          << (options_.request_full_detail
                                  ? detail_source_
                                  : QStringLiteral("<not requested>"));
        QTimer::singleShot(TERMINAL_SETTLE_MS, &application_, &QCoreApplication::quit);
    }

    void fail(const QString& reason) {
        if (stage_ == Stage::Finished || stage_ == Stage::Failed) {
            return;
        }
        const Stage failed_stage = stage_;
        stage_ = Stage::Failed;
        frame_poll_.stop();
        deadline_.stop();
        const QString current_source = editor_.previewSource();
        const QString before_source = editor_.beforePreviewSource();
        const auto* const workspace = precisionWorkspace(engine_);
        qCritical().noquote()
            << "Edit preview smoke failed:" << reason << "stage"
            << stageName(failed_stage) << "rows"
            << (review_.reviewModel() == nullptr ? -1
                                                 : review_.reviewModel()->rowCount())
            << "scanning" << review_.scanning() << "refreshing" << review_.refreshing()
            << "active" << editor_.active() << "state busy" << editor_.stateBusy()
            << "rendering" << editor_.rendering() << "current source" << current_source
            << "current histogram valid"
            << validEditHistogram(editor_.histogram(), current_source)
            << "before source" << before_source << "before histogram valid"
            << validEditHistogram(editor_.beforeHistogram(), before_source)
            << "QML generation"
            << (workspace == nullptr
                    ? QStringLiteral("<missing workspace>")
                    : workspace->property("readyPreviewGeneration").toString())
            << "QML before ready"
            << (workspace != nullptr
                && workspace->property("beforeFrameReady").toBool())
            << "detail mode" << editor_.detailMode() << "detail rendering"
            << editor_.detailRendering() << "full resolution preparing"
            << editor_.fullResolutionPreparing() << "full resolution ready"
            << editor_.fullResolutionReady() << "full resolution retained bytes"
            << editor_.fullResolutionRetainedBytes() << "detail geometry"
            << editor_.detailFullWidth() << "x" << editor_.detailFullHeight() << "tiles"
            << editor_.detailTiles().size() << "retained bytes"
            << editor_.detailRetainedBytes() << "detail error"
            << editor_.detailErrorText() << "status" << editor_.statusText();
        application_.exit(EXIT_FAILURE);
    }

    [[nodiscard]] static QString stageName(const Stage stage) {
        switch (stage) {
        case Stage::AwaitReviewItem:
            return QStringLiteral("await-review-item");
        case Stage::AwaitCurrentPreview:
            return QStringLiteral("await-current-preview");
        case Stage::AwaitBeforePreview:
            return QStringLiteral("await-before-preview");
        case Stage::AwaitFullDetail:
            return QStringLiteral("await-full-detail");
        case Stage::Finished:
            return QStringLiteral("finished");
        case Stage::Failed:
            return QStringLiteral("failed");
        }
        return QStringLiteral("unknown");
    }

    QCoreApplication& application_;
    QQmlApplicationEngine& engine_;
    ReviewController& review_;
    EditController& editor_;
    EditPreviewProvider* edit_preview_provider_ = nullptr;
    DesktopSmoke::EditPreviewSessionOptions options_;
    QTimer frame_poll_;
    QTimer deadline_;
    Stage stage_ = Stage::AwaitReviewItem;
    QString opened_photo_id_;
    QString opened_representation_id_;
    QString current_source_;
    QString before_source_;
    QString detail_source_;
};

} // namespace

void DesktopSmoke::startEditPreviewSession(
    QCoreApplication& application,
    QQmlApplicationEngine& engine,
    ReviewController& review,
    EditController& editor,
    EditPreviewProvider* const edit_preview_provider,
    const EditPreviewSessionOptions options
) {
    EditPreviewSession::start(
        application, engine, review, editor, edit_preview_provider, options
    );
}

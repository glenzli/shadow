#include "review_focus_detail_coordinator.hpp"

#include <QColorSpace>
#include <QDebug>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {

constexpr std::uint32_t REVIEW_FOCUS_DETAIL_SIDE = 384;
constexpr std::uint32_t REVIEW_FOCUS_TILE_SIDE = 1'024;

[[nodiscard]] QImage render_focus_detail(
    const std::shared_ptr<DesktopBackend>& backend,
    const ReviewFocusDetailRequest& request
) {
    const BackendPhotoEditState state =
        backend->photoEditState(request.photo_id, request.source_path);
    const BackendEditedDetailViewport viewport = backend->renderEditDetailViewport(
        request.photo_id,
        request.source_path,
        state.base_commit_id,
        state.grade_stack,
        request.render_token,
        request.center_x,
        request.center_y,
        REVIEW_FOCUS_DETAIL_SIDE,
        REVIEW_FOCUS_DETAIL_SIDE,
        REVIEW_FOCUS_TILE_SIDE,
        true
    );
    if (viewport.tiles.size() != 1) {
        throw std::runtime_error("review focus detail did not return one bounded tile");
    }
    const BackendEditedDetailTile& tile = viewport.tiles.constFirst();
    const quint64 expected_stride = static_cast<quint64>(tile.width) * 3U;
    const quint64 expected_bytes = expected_stride * tile.height;
    if (tile.width == 0 || tile.height == 0 || tile.row_stride_bytes != expected_stride
        || static_cast<quint64>(tile.bytes.size()) != expected_bytes) {
        throw std::runtime_error("review focus detail returned an invalid RGB8 tile");
    }
    const QImage borrowed(
        reinterpret_cast<const uchar*>(tile.bytes.constData()),
        static_cast<int>(tile.width),
        static_cast<int>(tile.height),
        static_cast<qsizetype>(tile.row_stride_bytes),
        QImage::Format_RGB888
    );
    QImage image = borrowed.copy();
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}

[[nodiscard]] ReviewFocusDetailTaskResult run_request(
    const ReviewFocusDetailCoordinator::Renderer& renderer,
    ReviewFocusDetailRequest request
) {
    ReviewFocusDetailTaskResult result;
    result.request = std::move(request);
    try {
        result.image = renderer(result.request);
        if (result.image.isNull()) {
            result.error = QStringLiteral("focus detail renderer returned no image");
        }
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

} // namespace

ReviewFocusDetailCoordinator::ReviewFocusDetailCoordinator(
    std::shared_ptr<DesktopBackend> backend,
    std::shared_ptr<ReviewFocusDetailStore> store,
    QObject* parent
) :
    ReviewFocusDetailCoordinator(
        [backend](const ReviewFocusDetailRequest& request) {
            return render_focus_detail(backend, request);
        },
        [backend]() { return backend->beginEditDetailRequest(); },
        std::move(store),
        parent
    ) {}

ReviewFocusDetailCoordinator::ReviewFocusDetailCoordinator(
    Renderer renderer,
    TokenFactory token_factory,
    std::shared_ptr<ReviewFocusDetailStore> store,
    QObject* parent
) :
    QObject(parent), renderer_(std::move(renderer)), token_factory_(std::move(token_factory)),
    store_(std::move(store)) {
    if (!renderer_ || !token_factory_ || !store_) {
        throw std::invalid_argument("review focus detail dependencies are required");
    }
    connect(
        &watcher_,
        &QFutureWatcher<ReviewFocusDetailTaskResult>::finished,
        this,
        &ReviewFocusDetailCoordinator::finish
    );
    debounce_timer_.setSingleShot(true);
    debounce_timer_.setInterval(120);
    connect(&debounce_timer_, &QTimer::timeout, this, &ReviewFocusDetailCoordinator::startPending);
}

ReviewFocusDetailCoordinator::~ReviewFocusDetailCoordinator() {
    debounce_timer_.stop();
    (void)token_factory_();
    watcher_.waitForFinished();
}

QString ReviewFocusDetailCoordinator::imageSource() const {
    return image_source_;
}

QString ReviewFocusDetailCoordinator::statusText() const {
    return status_text_;
}

bool ReviewFocusDetailCoordinator::busy() const noexcept {
    return busy_;
}

bool ReviewFocusDetailCoordinator::ready() const noexcept {
    return ready_;
}

bool ReviewFocusDetailCoordinator::failed() const noexcept {
    return failed_;
}

void ReviewFocusDetailCoordinator::request(
    const QString& photo_id,
    const QString& source_path,
    const double center_x,
    const double center_y
) {
    if (photo_id.isEmpty() || source_path.isEmpty() || !std::isfinite(center_x)
        || !std::isfinite(center_y)) {
        clear();
        return;
    }
    const quint64 generation = ++next_generation_;
    const std::uint64_t render_token = token_factory_();
    pending_request_ = ReviewFocusDetailRequest{
        .photo_id = photo_id,
        .source_path = source_path,
        .center_x = std::clamp(center_x, 0.0, 1.0),
        .center_y = std::clamp(center_y, 0.0, 1.0),
        .generation = generation,
        .render_token = render_token,
    };
    store_->clear(generation);
    resetPresentation();
    busy_ = true;
    status_text_ = tr("Preparing 100% focus detail…");
    emit stateChanged();
    if (!watcher_.isRunning()) {
        debounce_timer_.start();
    }
}

void ReviewFocusDetailCoordinator::clear() {
    const quint64 generation = ++next_generation_;
    (void)token_factory_();
    pending_request_.reset();
    debounce_timer_.stop();
    current_request_ = {};
    store_->clear(generation);
    resetPresentation();
    emit stateChanged();
}

void ReviewFocusDetailCoordinator::retranslateUi() {
    if (busy_) {
        status_text_ = tr("Preparing 100% focus detail…");
    } else if (ready_) {
        status_text_ = tr("100% focus detail ready");
    } else if (failed_) {
        status_text_ = tr("100% focus detail is unavailable");
    }
    emit stateChanged();
}

void ReviewFocusDetailCoordinator::startPending() {
    if (!pending_request_) {
        return;
    }
    current_request_ = *pending_request_;
    pending_request_.reset();
    watcher_.setFuture(QtConcurrent::run(run_request, renderer_, current_request_));
}

void ReviewFocusDetailCoordinator::finish() {
    const ReviewFocusDetailTaskResult result = watcher_.result();
    const bool is_current = result.request.generation == next_generation_;
    if (pending_request_) {
        debounce_timer_.start();
        return;
    }
    if (!is_current) {
        return;
    }
    busy_ = false;
    if (!result.error.isEmpty()) {
        qWarning().noquote() << "Review focus detail failed:" << result.error;
        failed_ = true;
        ready_ = false;
        image_source_.clear();
        status_text_ = tr("100% focus detail is unavailable");
    } else {
        store_->publish(result.request.generation, result.image);
        failed_ = false;
        ready_ = true;
        image_source_ = QStringLiteral("image://shadow-review-detail/focus?generation=%1")
                            .arg(result.request.generation);
        status_text_ = tr("100% focus detail ready");
    }
    emit stateChanged();
}

void ReviewFocusDetailCoordinator::resetPresentation() {
    image_source_.clear();
    status_text_.clear();
    busy_ = false;
    ready_ = false;
    failed_ = false;
}

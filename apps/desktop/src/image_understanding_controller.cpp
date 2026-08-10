#include "image_understanding_controller.hpp"

#include <QtConcurrentRun>

#include <QDebug>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

ImageUnderstandingController::ImageUnderstandingController(
    AiPreferences* const preferences,
    BatchRunner batch_runner,
    SnapshotLoader snapshot_loader,
    PauseWriter pause_writer,
    ReviewRunner review_runner,
    ReviewAccepter review_accepter,
    ReviewDismisser review_dismisser,
    ProposalLoader proposal_loader,
    KeywordAccepter keyword_accepter,
    CategoryProvider category_provider,
    TaxonomyRevisionProvider taxonomy_revision_provider,
    QObject* const parent
) :
    QObject(parent), preferences_(preferences), batch_runner_(std::move(batch_runner)),
    snapshot_loader_(std::move(snapshot_loader)), pause_writer_(std::move(pause_writer)),
    review_runner_(std::move(review_runner)), review_accepter_(std::move(review_accepter)),
    review_dismisser_(std::move(review_dismisser)),
    proposal_loader_(std::move(proposal_loader)), keyword_accepter_(std::move(keyword_accepter)),
    category_provider_(std::move(category_provider)),
    taxonomy_revision_provider_(std::move(taxonomy_revision_provider)) {
    Q_ASSERT(preferences_ != nullptr);
    next_batch_timer_.setSingleShot(true);
    next_batch_timer_.setInterval(450);
    connect(&next_batch_timer_, &QTimer::timeout, this, &ImageUnderstandingController::runNextBatch);
    connect(
        &batch_watcher_,
        &QFutureWatcher<ImageUnderstandingTaskResult>::finished,
        this,
        &ImageUnderstandingController::finishBatch
    );
    connect(
        &review_watcher_,
        &QFutureWatcher<AdvancedClassificationTaskResult>::finished,
        this,
        &ImageUnderstandingController::finishAdvancedReview
    );
    const auto reconcile = [this]() { reconcileBackgroundPolicy(); };
    connect(
        preferences_,
        &AiPreferences::imageUnderstandingExecutionAllowedChanged,
        this,
        reconcile
    );
    connect(
        preferences_,
        &AiPreferences::imageUnderstandingBackgroundEnabledChanged,
        this,
        reconcile
    );
    const auto policy_changed = [this]() {
        policy_changed_ = true;
        reconcileBackgroundPolicy();
    };
    connect(
        preferences_,
        &AiPreferences::imageUnderstandingScanScopeChanged,
        this,
        policy_changed
    );
    connect(
        preferences_,
        &AiPreferences::imageUnderstandingMinimumRatingChanged,
        this,
        policy_changed
    );
    restoreSnapshot();
    QTimer::singleShot(0, this, &ImageUnderstandingController::reconcileBackgroundPolicy);
}

ImageUnderstandingController::~ImageUnderstandingController() {
    shutting_down_ = true;
    next_batch_timer_.stop();
    batch_watcher_.waitForFinished();
    review_watcher_.waitForFinished();
    if (state_ == State::Running && !generation_.isEmpty()) {
        try {
            pause_writer_(generation_);
        } catch (const std::exception& error) {
            qWarning().noquote() << "Could not checkpoint photo understanding during shutdown:"
                                 << error.what();
        }
    }
}

bool ImageUnderstandingController::busy() const noexcept {
    return batch_watcher_.isRunning() || state_ == State::Running;
}

bool ImageUnderstandingController::canPause() const noexcept {
    return state_ == State::Running;
}

bool ImageUnderstandingController::canResume() const noexcept {
    return executionAllowed() && !generation_.isEmpty()
        && (state_ == State::Paused || state_ == State::Failed);
}

int ImageUnderstandingController::progressPercent() const noexcept {
    if (total_photos_ == 0)
        return 0;
    return static_cast<int>(
        std::min<std::uint64_t>(100, (processed_photos_ * 100) / total_photos_)
    );
}

qulonglong ImageUnderstandingController::processedPhotos() const noexcept {
    return processed_photos_;
}

qulonglong ImageUnderstandingController::totalPhotos() const noexcept {
    return total_photos_;
}

QString ImageUnderstandingController::statusText() const {
    switch (state_) {
    case State::Disabled:
        return tr("Local photo understanding is disabled.");
    case State::Idle:
        return backgroundAllowed() ? tr("Eligible photos are waiting for local analysis.")
                                   : tr("Background photo understanding is off.");
    case State::Running:
        return pause_requested_ ? tr("Pausing after the current photo…")
                                : tr("Understanding photos locally… %1 of %2")
                                      .arg(processed_photos_)
                                      .arg(total_photos_);
    case State::Paused:
        return tr("Photo understanding is paused at %1 of %2.")
            .arg(processed_photos_)
            .arg(total_photos_);
    case State::Complete:
        return tr("Eligible photos are up to date.");
    case State::Failed:
        return tr("Photo understanding was interrupted.");
    }
    return {};
}

QString ImageUnderstandingController::errorText() const {
    if (state_ != State::Failed)
        return {};
    if (diagnostic_.contains(QStringLiteral("connect"), Qt::CaseInsensitive))
        return tr("Infer Runtime is unavailable. Start it, then resume.");
    return tr("The checkpoint is safe. Resume after the local model is available.");
}

bool ImageUnderstandingController::advancedReviewBusy() const noexcept {
    return review_watcher_.isRunning();
}

QString ImageUnderstandingController::advancedReviewPhotoId() const {
    return advanced_photo_id_;
}

QString ImageUnderstandingController::advancedReviewRepresentationId() const {
    return advanced_representation_id_;
}

QString ImageUnderstandingController::advancedReviewDisposition() const {
    return advanced_disposition_;
}

QString ImageUnderstandingController::advancedReviewCategoryId() const {
    return advanced_category_id_;
}

QString ImageUnderstandingController::advancedReviewCategoryName() const {
    return advanced_category_name_;
}

QString ImageUnderstandingController::advancedReviewError() const {
    if (advanced_diagnostic_.isEmpty())
        return {};
    return tr("The advanced local review could not finish. Check Infer Runtime and try again.");
}

bool ImageUnderstandingController::photoProposalAvailable() const noexcept {
    return photo_proposal_.available;
}

QString ImageUnderstandingController::photoProposalPhotoId() const {
    return photo_proposal_.photo_id;
}

QString ImageUnderstandingController::photoProposalRepresentationId() const {
    return photo_proposal_.representation_id;
}

QString ImageUnderstandingController::photoDescription() const {
    return photo_proposal_.description;
}

QStringList ImageUnderstandingController::photoKeywords() const {
    return photo_proposal_.keywords;
}

QString ImageUnderstandingController::photoProposalDisposition() const {
    return photo_proposal_.disposition;
}

void ImageUnderstandingController::refresh() {
    if (!executionAllowed() || batch_watcher_.isRunning())
        return;
    if (!preferences_->imageUnderstandingBackgroundEnabled())
        preferences_->setImageUnderstandingBackgroundEnabled(true);
    if (batch_watcher_.isRunning())
        return;
    startRun(true);
}

void ImageUnderstandingController::resume() {
    if (!canResume() || batch_watcher_.isRunning())
        return;
    if (!preferences_->imageUnderstandingBackgroundEnabled())
        preferences_->setImageUnderstandingBackgroundEnabled(true);
    if (batch_watcher_.isRunning())
        return;
    startRun(false);
}

void ImageUnderstandingController::pause() {
    if (state_ != State::Running)
        return;
    pause_requested_ = true;
    next_batch_timer_.stop();
    emit stateChanged();
    if (!batch_watcher_.isRunning() && !generation_.isEmpty()) {
        try {
            const BackendImageUnderstandingSnapshot snapshot = pause_writer_(generation_);
            processed_photos_ = snapshot.processed_photos;
            total_photos_ = snapshot.total_photos;
            state_ = State::Paused;
            emit stateChanged();
        } catch (const std::exception& error) {
            diagnostic_ = QString::fromUtf8(error.what());
            state_ = State::Failed;
            emit stateChanged();
        }
    }
}

void ImageUnderstandingController::notifyLibraryChanged() {
    policy_changed_ = true;
    if (backgroundAllowed() && !busy())
        startRun(true);
}

void ImageUnderstandingController::requestAdvancedReview(
    const QString& photo_id,
    const QString& representation_id
) {
    if (!executionAllowed() || review_watcher_.isRunning() || photo_id.isEmpty()
        || representation_id.isEmpty()) {
        return;
    }
    const QVector<BackendClassificationReviewCategory> categories = category_provider_();
    const QString taxonomy_revision = taxonomy_revision_provider_();
    if (categories.isEmpty() || taxonomy_revision.isEmpty())
        return;
    clearAdvancedReview();
    advanced_photo_id_ = photo_id;
    advanced_representation_id_ = representation_id;
    emit advancedReviewChanged();
    review_watcher_.setFuture(QtConcurrent::run(
        [runner = review_runner_, photo_id, representation_id, taxonomy_revision, categories]() {
            AdvancedClassificationTaskResult result;
            try {
                result.proposal =
                    runner(photo_id, representation_id, taxonomy_revision, categories);
            } catch (const std::exception& error) {
                result.diagnostic = QString::fromUtf8(error.what());
            }
            return result;
        }
    ));
}

void ImageUnderstandingController::acceptAdvancedReview() {
    if (review_watcher_.isRunning() || advanced_source_revision_.isEmpty()
        || advanced_disposition_ != QStringLiteral("matched")) {
        return;
    }
    try {
        const QString accepted = review_accepter_(
            advanced_photo_id_,
            advanced_representation_id_,
            advanced_source_revision_
        );
        if (accepted != advanced_category_id_)
            throw std::runtime_error("advanced review category changed during acceptance");
        advanced_disposition_ = QStringLiteral("accepted");
        emit advancedReviewChanged();
        emit advancedReviewAccepted();
    } catch (const std::exception& error) {
        advanced_diagnostic_ = QString::fromUtf8(error.what());
        emit advancedReviewChanged();
    }
}

void ImageUnderstandingController::dismissAdvancedReview() {
    if (review_watcher_.isRunning() || advanced_source_revision_.isEmpty())
        return;
    try {
        review_dismisser_(
            advanced_photo_id_,
            advanced_representation_id_,
            advanced_source_revision_
        );
        advanced_disposition_ = QStringLiteral("dismissed");
        emit advancedReviewChanged();
    } catch (const std::exception& error) {
        advanced_diagnostic_ = QString::fromUtf8(error.what());
        emit advancedReviewChanged();
    }
}

void ImageUnderstandingController::clearAdvancedReview() {
    if (review_watcher_.isRunning())
        return;
    advanced_photo_id_.clear();
    advanced_representation_id_.clear();
    advanced_source_revision_.clear();
    advanced_disposition_.clear();
    advanced_category_id_.clear();
    advanced_category_name_.clear();
    advanced_diagnostic_.clear();
    emit advancedReviewChanged();
}

void ImageUnderstandingController::loadPhotoProposal(
    const QString& photo_id,
    const QString& representation_id
) {
    try {
        photo_proposal_ = proposal_loader_(photo_id, representation_id);
    } catch (const std::exception& error) {
        qWarning().noquote() << "Could not load photo-understanding proposal:" << error.what();
        photo_proposal_ = {};
    }
    emit photoProposalChanged();
}

void ImageUnderstandingController::acceptPhotoKeywords() {
    if (!photo_proposal_.available || photo_proposal_.source_revision.isEmpty()
        || photo_proposal_.disposition != QStringLiteral("suggested")) {
        return;
    }
    try {
        keyword_accepter_(
            photo_proposal_.photo_id,
            photo_proposal_.representation_id,
            photo_proposal_.source_revision
        );
        photo_proposal_.disposition = QStringLiteral("accepted");
        emit photoProposalChanged();
    } catch (const std::exception& error) {
        qWarning().noquote() << "Could not accept photo keyword suggestions:" << error.what();
    }
}

void ImageUnderstandingController::retranslateUi() {
    emit stateChanged();
    emit advancedReviewChanged();
}

void ImageUnderstandingController::restoreSnapshot() {
    try {
        const BackendImageUnderstandingSnapshot snapshot = snapshot_loader_();
        generation_ = snapshot.generation;
        policy_revision_ = snapshot.policy_revision;
        processed_photos_ = snapshot.processed_photos;
        total_photos_ = snapshot.total_photos;
        diagnostic_.clear();
        pause_requested_ = false;
        if (!executionAllowed())
            state_ = State::Disabled;
        else if (!snapshot.available)
            state_ = State::Idle;
        else if (snapshot.policy_revision != expectedPolicyRevision()) {
            state_ = State::Idle;
            policy_changed_ = true;
        } else if (snapshot.status == QStringLiteral("complete"))
            state_ = State::Complete;
        else if (snapshot.status == QStringLiteral("failed"))
            state_ = State::Failed;
        else
            state_ = State::Paused;
        emit stateChanged();
    } catch (const std::exception& error) {
        diagnostic_ = QString::fromUtf8(error.what());
        state_ = State::Failed;
        emit stateChanged();
    }
}

void ImageUnderstandingController::reconcileBackgroundPolicy() {
    if (!executionAllowed()) {
        if (state_ == State::Running)
            pause();
        state_ = State::Disabled;
        emit stateChanged();
        return;
    }
    if (!backgroundAllowed()) {
        if (state_ == State::Running)
            pause();
        else if (state_ == State::Disabled)
            restoreSnapshot();
        return;
    }
    if (batch_watcher_.isRunning())
        return;
    if (policy_changed_ || state_ == State::Idle) {
        startRun(true);
    } else if (state_ == State::Paused || state_ == State::Failed) {
        startRun(false);
    }
}

void ImageUnderstandingController::startRun(const bool start_new) {
    if (!executionAllowed() || batch_watcher_.isRunning())
        return;
    if (start_new) {
        generation_.clear();
        processed_photos_ = 0;
        total_photos_ = 0;
    }
    diagnostic_.clear();
    pause_requested_ = false;
    first_batch_ = start_new;
    policy_changed_ = false;
    state_ = State::Running;
    emit stateChanged();
    runNextBatch();
}

void ImageUnderstandingController::runNextBatch() {
    if (shutting_down_ || pause_requested_ || !backgroundAllowed()) {
        pause();
        return;
    }
    const QString scope = preferences_->imageUnderstandingScanScope();
    const int minimum_rating = preferences_->imageUnderstandingMinimumRating();
    const QString generation = generation_;
    const bool start_new = first_batch_;
    const bool auto_apply = preferences_->imageUnderstandingAutoApplyKeywords();
    first_batch_ = false;
    batch_watcher_.setFuture(QtConcurrent::run(
        [runner = batch_runner_, scope, minimum_rating, generation, start_new, auto_apply]() {
            ImageUnderstandingTaskResult result;
            try {
                result.batch = runner(scope, minimum_rating, generation, start_new, auto_apply);
            } catch (const std::exception& error) {
                result.diagnostic = QString::fromUtf8(error.what());
            }
            return result;
        }
    ));
}

void ImageUnderstandingController::finishBatch() {
    if (shutting_down_)
        return;
    const ImageUnderstandingTaskResult result = batch_watcher_.result();
    if (!result.diagnostic.isEmpty()) {
        diagnostic_ = result.diagnostic;
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    generation_ = result.batch.generation;
    processed_photos_ = result.batch.processed_photos;
    total_photos_ = result.batch.total_photos;
    emit stateChanged();
    if (result.batch.status == QStringLiteral("complete")) {
        state_ = State::Complete;
        emit stateChanged();
        return;
    }
    if (pause_requested_ || !backgroundAllowed()) {
        pause();
        return;
    }
    next_batch_timer_.start();
}

void ImageUnderstandingController::finishAdvancedReview() {
    const AdvancedClassificationTaskResult result = review_watcher_.result();
    if (!result.diagnostic.isEmpty()) {
        advanced_diagnostic_ = result.diagnostic;
        emit advancedReviewChanged();
        return;
    }
    setAdvancedProposal(result.proposal);
}

QString ImageUnderstandingController::expectedPolicyRevision() const {
    const QString scope = preferences_->imageUnderstandingScanScope();
    const bool uses_rating = scope == QStringLiteral("minimum_rating")
        || scope == QStringLiteral("liked_or_minimum_rating");
    return QStringLiteral("shadow.image-understanding-scan:v1:%1:rating-%2:batch-4")
        .arg(scope, uses_rating ? QString::number(preferences_->imageUnderstandingMinimumRating())
                                : QStringLiteral("none"));
}

bool ImageUnderstandingController::executionAllowed() const noexcept {
    return preferences_->imageUnderstandingExecutionAllowed();
}

bool ImageUnderstandingController::backgroundAllowed() const noexcept {
    return executionAllowed() && preferences_->imageUnderstandingBackgroundEnabled();
}

void ImageUnderstandingController::setAdvancedProposal(
    const BackendClassificationReviewProposal& proposal
) {
    if (!proposal.available) {
        advanced_diagnostic_ = QStringLiteral("advanced review returned no proposal");
        emit advancedReviewChanged();
        return;
    }
    advanced_source_revision_ = proposal.source_revision;
    advanced_disposition_ = proposal.disposition;
    advanced_category_id_ = proposal.category_id;
    advanced_category_name_.clear();
    for (const BackendClassificationReviewCategory& category : category_provider_()) {
        if (category.id == advanced_category_id_) {
            advanced_category_name_ = category.name;
            break;
        }
    }
    advanced_diagnostic_.clear();
    emit advancedReviewChanged();
}

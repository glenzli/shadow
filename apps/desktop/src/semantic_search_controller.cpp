#include "semantic_search_controller.hpp"

#include <QtConcurrentRun>

#include <QCryptographicHash>
#include <QDebug>
#include <QLocale>
#include <QVariantMap>

#include <algorithm>
#include <exception>
#include <utility>

namespace {

struct RelevanceProjection final {
    qsizetype high_count = 0;
    qsizetype possible_count = 0;
};

RelevanceProjection classifyRelevance(const BackendSemanticSearchReport& report) {
    // The model supplies relative similarity, not calibrated relevance. Keep
    // every ranked candidate visible without assigning any strong-match badge.
    return {.high_count = 0, .possible_count = report.matches.size()};
}

QString relevanceTierForIndex(const qsizetype index, const RelevanceProjection projection) {
    if (index < projection.high_count) {
        return QStringLiteral("high");
    }
    if (index < projection.high_count + projection.possible_count) {
        return QStringLiteral("possible");
    }
    return QStringLiteral("hidden-low");
}

uint boundedCount(const qsizetype count) noexcept {
    return static_cast<uint>(std::max<qsizetype>(0, count));
}

QStringList representationKeys(
    const BackendSemanticSearchReport& report,
    const qsizetype first_index,
    const qsizetype count
) {
    QStringList keys;
    keys.reserve(count);
    const qsizetype end = std::min(report.matches.size(), first_index + count);
    for (qsizetype index = first_index; index < end; ++index) {
        const BackendSemanticSearchMatch& semantic_match = report.matches.at(index);
        keys.push_back(semantic_match.photo_id + QChar{0x001f} + semantic_match.representation_id);
    }
    return keys;
}

} // namespace

SemanticSearchController::SemanticSearchController(
    Runner runner,
    Begin begin,
    Cancel cancel,
    QObject* const parent
) :
    QObject(parent), runner_(std::move(runner)), begin_(std::move(begin)),
    cancel_(std::move(cancel)) {
    connect(
        &watcher_,
        &QFutureWatcher<SemanticSearchTaskResult>::finished,
        this,
        &SemanticSearchController::finishSearch
    );
}

SemanticSearchController::~SemanticSearchController() {
    if (request_in_flight_ && cancel_) {
        cancel_(active_token_);
    }
    watcher_.waitForFinished();
}

bool SemanticSearchController::busy() const noexcept {
    return request_in_flight_;
}

bool SemanticSearchController::hasResults() const noexcept {
    return has_results_;
}

QString SemanticSearchController::activeQuery() const {
    return active_query_;
}

QString SemanticSearchController::statusText() const {
    switch (state_) {
    case State::Idle:
        return tr("Describe a photo to search the current Library preview.");
    case State::Running:
        return tr("Comparing your description with local photos…");
    case State::Ready:
        return tr("Semantic search finished.");
    case State::Failed:
        return tr("Semantic search could not finish.");
    }
    return {};
}

QString SemanticSearchController::errorText() const {
    if (state_ != State::Failed) {
        return {};
    }
    return tr(
        "Make sure Infer Runtime is running and Shadow access is configured, then try again."
    );
}

QVariantList SemanticSearchController::matches() const {
    const RelevanceProjection projection = classifyRelevance(report_);
    QVariantList projected;
    projected.reserve(report_.matches.size());
    for (qsizetype index = 0; index < report_.matches.size(); ++index) {
        const BackendSemanticSearchMatch& semantic_match = report_.matches.at(index);
        projected.push_back(
            QVariantMap{
                {QStringLiteral("rank"), index + 1},
                {QStringLiteral("photoId"), semantic_match.photo_id},
                {QStringLiteral("representationId"), semantic_match.representation_id},
                {QStringLiteral("similarity"), semantic_match.cosine_similarity},
                {QStringLiteral("relevance"), relevanceTierForIndex(index, projection)},
            }
        );
    }
    return projected;
}

QStringList SemanticSearchController::rankedRepresentationKeys() const {
    const RelevanceProjection projection = classifyRelevance(report_);
    return representationKeys(report_, 0, projection.high_count + projection.possible_count);
}

QStringList SemanticSearchController::highRepresentationKeys() const {
    return representationKeys(report_, 0, classifyRelevance(report_).high_count);
}

QStringList SemanticSearchController::possibleRepresentationKeys() const {
    const RelevanceProjection projection = classifyRelevance(report_);
    return representationKeys(report_, projection.high_count, projection.possible_count);
}

uint SemanticSearchController::highRelevanceCount() const noexcept {
    return boundedCount(classifyRelevance(report_).high_count);
}

uint SemanticSearchController::possibleRelevanceCount() const noexcept {
    return boundedCount(classifyRelevance(report_).possible_count);
}

uint SemanticSearchController::hiddenLowRelevanceCount() const noexcept {
    const RelevanceProjection projection = classifyRelevance(report_);
    return boundedCount(report_.matches.size() - projection.high_count - projection.possible_count);
}

uint SemanticSearchController::shownResultCount() const noexcept {
    const RelevanceProjection projection = classifyRelevance(report_);
    return boundedCount(projection.high_count + projection.possible_count);
}

uint SemanticSearchController::consideredPhotos() const noexcept {
    return report_.considered_photos;
}

uint SemanticSearchController::embeddedPhotos() const noexcept {
    return report_.embedded_photos;
}

uint SemanticSearchController::skippedItems() const noexcept {
    return report_.skipped_items;
}

bool SemanticSearchController::truncated() const noexcept {
    return report_.truncated;
}

void SemanticSearchController::search(const QString& query) {
    const QString normalized_query = query.trimmed();
    if (normalized_query.isEmpty()) {
        return;
    }

    if (request_in_flight_) {
        pending_query_ = normalized_query;
        discard_result_ = true;
        if (cancel_)
            cancel_(active_token_);
        return;
    }
    discard_result_ = false;
    try {
        active_token_ = begin_ ? begin_() : active_token_ + 1;
    } catch (const std::exception&) {
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    const QString language = QLocale().bcp47Name();
    QByteArray revision_input = normalized_query.toUtf8();
    revision_input.push_back('\0');
    revision_input.append(language.toUtf8());
    const QString query_revision =
        QStringLiteral("shadow:semantic-search-ui/query:%1")
            .arg(
                QString::fromLatin1(
                    QCryptographicHash::hash(revision_input, QCryptographicHash::Sha256).toHex()
                )
            );
    report_ = {};
    active_query_.clear();
    has_results_ = false;
    state_ = State::Running;
    request_in_flight_ = true;
    watcher_.setFuture(
        QtConcurrent::run([runner = runner_,
                           normalized_query,
                           query_revision,
                           language,
                           token = active_token_]() {
            SemanticSearchTaskResult result;
            result.query = normalized_query;
            try {
                result.report = runner(normalized_query, query_revision, language, token);
            } catch (const std::exception& error) {
                result.diagnostic = QString::fromUtf8(error.what());
            }
            return result;
        })
    );
    emit resultsChanged();
    emit stateChanged();
}

void SemanticSearchController::clearSessionResults() {
    pending_query_.clear();
    discard_result_ = true;
    if (request_in_flight_ && cancel_)
        cancel_(active_token_);
    report_ = {};
    active_query_.clear();
    has_results_ = false;
    state_ = State::Idle;
    emit resultsChanged();
    emit stateChanged();
}

void SemanticSearchController::retranslateUi() {
    emit stateChanged();
}

void SemanticSearchController::finishSearch() {
    const SemanticSearchTaskResult result = watcher_.result();
    request_in_flight_ = false;
    if (discard_result_) {
        const QString pending = std::exchange(pending_query_, {});
        state_ = State::Idle;
        emit stateChanged();
        if (!pending.isEmpty())
            search(pending);
        return;
    }
    if (!result.diagnostic.isEmpty()) {
        qWarning().noquote() << "Semantic search failed:" << result.diagnostic;
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    report_ = result.report;
    active_query_ = result.query;
    has_results_ = true;
    state_ = State::Ready;
    emit resultsChanged();
    emit stateChanged();
}

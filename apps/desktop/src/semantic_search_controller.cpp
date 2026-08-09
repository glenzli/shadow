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

constexpr qsizetype MAXIMUM_HIGH_RELEVANCE_MATCHES = 6;
constexpr qsizetype MAXIMUM_VISIBLE_MATCHES = 16;
constexpr float HIGH_RELEVANCE_SCORE_WINDOW = 0.04F;
constexpr float POSSIBLE_RELEVANCE_SCORE_WINDOW = 0.10F;

struct RelevanceProjection final {
    qsizetype high_count = 0;
    qsizetype possible_count = 0;
};

RelevanceProjection classifyRelevance(const BackendSemanticSearchReport& report) {
    if (report.matches.isEmpty()) {
        return {};
    }

    const qsizetype match_count = report.matches.size();
    const qsizetype high_rank_limit =
        std::min(MAXIMUM_HIGH_RELEVANCE_MATCHES, std::max<qsizetype>(1, (match_count + 2) / 3));
    const qsizetype visible_rank_limit =
        std::min(MAXIMUM_VISIBLE_MATCHES, std::max<qsizetype>(1, (match_count * 2 + 2) / 3));
    const float top_score = report.matches.front().cosine_similarity;

    qsizetype high_count = 1;
    while (high_count < high_rank_limit
           && report.matches.at(high_count).cosine_similarity
                  >= top_score - HIGH_RELEVANCE_SCORE_WINDOW) {
        ++high_count;
    }

    qsizetype visible_count = high_count;
    while (visible_count < visible_rank_limit
           && report.matches.at(visible_count).cosine_similarity
                  >= top_score - POSSIBLE_RELEVANCE_SCORE_WINDOW) {
        ++visible_count;
    }
    return {
        .high_count = high_count,
        .possible_count = visible_count - high_count,
    };
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

} // namespace

SemanticSearchController::SemanticSearchController(Runner runner, QObject* const parent) :
    QObject(parent), runner_(std::move(runner)) {
    connect(
        &watcher_,
        &QFutureWatcher<SemanticSearchTaskResult>::finished,
        this,
        &SemanticSearchController::finishSearch
    );
}

SemanticSearchController::~SemanticSearchController() {
    watcher_.waitForFinished();
}

bool SemanticSearchController::busy() const noexcept {
    return watcher_.isRunning();
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
    qsizetype first_index = 0;
    qsizetype visible_count = projection.high_count + projection.possible_count;
    if (relevance_filter_ == QStringLiteral("high")) {
        visible_count = projection.high_count;
    } else if (relevance_filter_ == QStringLiteral("possible")) {
        first_index = projection.high_count;
        visible_count = projection.possible_count;
    }

    QStringList keys;
    keys.reserve(visible_count);
    for (qsizetype index = first_index; index < first_index + visible_count; ++index) {
        const BackendSemanticSearchMatch& semantic_match = report_.matches.at(index);
        keys.push_back(semantic_match.photo_id + QChar{0x001f} + semantic_match.representation_id);
    }
    return keys;
}

QString SemanticSearchController::relevanceFilter() const {
    return relevance_filter_;
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
    if (relevance_filter_ == QStringLiteral("high")) {
        return boundedCount(projection.high_count);
    }
    if (relevance_filter_ == QStringLiteral("possible")) {
        return boundedCount(projection.possible_count);
    }
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
    if (watcher_.isRunning()) {
        return;
    }
    const QString normalized_query = query.trimmed();
    if (normalized_query.isEmpty()) {
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
    relevance_filter_ = QStringLiteral("all");
    has_results_ = false;
    state_ = State::Running;
    watcher_.setFuture(
        QtConcurrent::run([runner = runner_, normalized_query, query_revision, language]() {
            SemanticSearchTaskResult result;
            result.query = normalized_query;
            try {
                result.report = runner(normalized_query, query_revision, language);
            } catch (const std::exception& error) {
                result.diagnostic = QString::fromUtf8(error.what());
            }
            return result;
        })
    );
    emit resultsChanged();
    emit stateChanged();
}

void SemanticSearchController::setRelevanceFilter(const QString& filter) {
    if (!has_results_ || watcher_.isRunning()) {
        return;
    }
    const QString normalized_filter = filter.trimmed().toLower();
    if (normalized_filter != QStringLiteral("all") && normalized_filter != QStringLiteral("high")
        && normalized_filter != QStringLiteral("possible")) {
        return;
    }
    if (normalized_filter == relevance_filter_) {
        return;
    }
    relevance_filter_ = normalized_filter;
    emit resultsChanged();
}

void SemanticSearchController::clearSessionResults() {
    if (watcher_.isRunning()) {
        return;
    }
    report_ = {};
    active_query_.clear();
    relevance_filter_ = QStringLiteral("all");
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

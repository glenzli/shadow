#include "smart_category_controller.hpp"

#include <QtConcurrentRun>

#include <QCryptographicHash>
#include <QDebug>
#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <utility>

namespace {
constexpr float DEFAULT_MINIMUM_SIMILARITY = 0.05F;
constexpr int SMART_CATEGORY_SETTINGS_VERSION = 3;

std::optional<float> calibratedBuiltInMinimumSimilarity(const QStringView id) {
    if (id == u"portrait" || id == u"landscape" || id == u"travel" || id == u"night")
        return 0.07F;
    if (id == u"street" || id == u"architecture")
        return 0.06F;
    if (id == u"animals")
        return 0.06F;
    if (id == u"food" || id == u"sports")
        return DEFAULT_MINIMUM_SIMILARITY;
    return std::nullopt;
}

QString generatedCategoryId(const QString& name, const QString& description) {
    QByteArray source = name.trimmed().toUtf8();
    source.push_back('\0');
    source.append(description.trimmed().toUtf8());
    return QStringLiteral("custom-%1")
        .arg(
            QString::fromLatin1(
                QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex().left(16)
            )
        );
}
} // namespace

SmartCategoryController::SmartCategoryController(
    Runner runner,
    SnapshotLoader snapshot_loader,
    MembersLoader members_loader,
    ReviewQueueLoader review_queue_loader,
    FeedbackWriter feedback_writer,
    PauseWriter pause_writer,
    QObject* const parent
) :
    QObject(parent), runner_(std::move(runner)), snapshot_loader_(std::move(snapshot_loader)),
    members_loader_(std::move(members_loader)),
    review_queue_loader_(std::move(review_queue_loader)),
    feedback_writer_(std::move(feedback_writer)), pause_writer_(std::move(pause_writer)) {
    loadSettings();
    connect(
        &watcher_,
        &QFutureWatcher<SmartCategoryTaskResult>::finished,
        this,
        &SmartCategoryController::finishBatch
    );
    feedback_refresh_timer_.setSingleShot(true);
    feedback_refresh_timer_.setInterval(1800);
    connect(&feedback_refresh_timer_, &QTimer::timeout, this, [this]() { refresh(); });
    restoreSnapshot();
    if (adaptation_pending_) {
        QTimer::singleShot(0, this, [this]() { continuePendingAdaptation(); });
    }
}

SmartCategoryController::~SmartCategoryController() {
    watcher_.waitForFinished();
    if (state_ == State::Running && !generation_.isEmpty()) {
        try {
            pause_writer_(generation_);
        } catch (const std::exception& error) {
            qWarning().noquote() << "Could not checkpoint smart categories during shutdown:"
                                 << error.what();
        }
    }
}

QVector<SmartCategoryController::Category> SmartCategoryController::defaultCategories() {
    return {
        {"portrait",
         tr("Portrait"),
         "a portrait photograph focused on one or more people",
         0.07F,
         true},
        {"landscape",
         tr("Landscape"),
         "a landscape photograph of nature, mountains, coast, countryside, or a wide outdoor scene",
         0.07F,
         true},
        {"street",
         tr("Street"),
         "a street photography scene showing candid urban life",
         0.06F,
         true},
        {"architecture",
         tr("Architecture"),
         "an architecture photograph focused on a building, interior, or designed structure",
         0.06F,
         true},
        {"animals",
         tr("Animals"),
         "a photograph focused on an animal, bird, wildlife, or pet",
         0.06F,
         true},
        {"travel",
         tr("Travel"),
         "a travel photograph showing a destination, journey, landmark, or local culture",
         0.07F,
         true},
        {"food",
         tr("Food & Still Life"),
         "a food, product, tabletop, or still life photograph",
         DEFAULT_MINIMUM_SIMILARITY,
         true},
        {"sports",
         tr("Sports & Action"),
         "a sports or action photograph with movement",
         DEFAULT_MINIMUM_SIMILARITY,
         true},
        {"night",
         tr("Night & Astro"),
         "a night photograph, city lights, stars, or astrophotography scene",
         0.07F,
         true},
    };
}

QVariantList SmartCategoryController::categories() const {
    QVariantList result;
    result.reserve(categories_.size());
    for (const Category& category : categories_) {
        result.push_back(
            QVariantMap{
                {"id", category.id},
                {"name", category.name},
                {"description", category.description},
                {"minimumSimilarity", category.minimum_similarity},
                {"enabled", category.enabled},
                {"count", category_counts_.value(category.id)},
                {"selected", category.id == selected_category_id_},
            }
        );
    }
    return result;
}

bool SmartCategoryController::busy() const noexcept {
    return watcher_.isRunning() || state_ == State::Running;
}
bool SmartCategoryController::canPause() const noexcept {
    return state_ == State::Running;
}
bool SmartCategoryController::canResume() const noexcept {
    return (state_ == State::Paused || state_ == State::Failed) && !generation_.isEmpty();
}
bool SmartCategoryController::needsUpdate() const noexcept {
    return state_ == State::Idle || state_ == State::NeedsUpdate;
}
bool SmartCategoryController::failed() const noexcept {
    return state_ == State::Failed;
}
bool SmartCategoryController::hasPublishedResults() const noexcept {
    return has_published_results_;
}
bool SmartCategoryController::hasMatches() const noexcept {
    return std::any_of(
        category_counts_.cbegin(),
        category_counts_.cend(),
        [](const qulonglong count) { return count > 0; }
    );
}
qulonglong SmartCategoryController::processedPhotos() const noexcept {
    return processed_photos_;
}
qulonglong SmartCategoryController::totalPhotos() const noexcept {
    return total_photos_;
}
int SmartCategoryController::progressPercent() const noexcept {
    if (total_photos_ == 0)
        return 0;
    return static_cast<int>(
        std::min<std::uint64_t>(100, (processed_photos_ * 100) / total_photos_)
    );
}

QString SmartCategoryController::statusText() const {
    switch (state_) {
    case State::Idle:
    case State::NeedsUpdate:
        if (adaptation_pending_)
            return tr("Corrections are waiting to update similar photos.");
        return has_published_results_ ? tr("New photos are waiting for smart classification.")
                                      : tr("Smart categories are ready to be built locally.");
    case State::Running:
        return pause_requested_ ? tr("Pausing after the current batch…")
                                : tr("Classifying photos locally… %1 of %2")
                                      .arg(processed_photos_)
                                      .arg(total_photos_);
    case State::Paused:
        return tr("Smart classification is paused at %1 of %2.")
            .arg(processed_photos_)
            .arg(total_photos_);
    case State::Ready:
        return hasMatches() ? tr("Smart categories are up to date.")
                            : tr("Analysis finished, but no photos meet the current thresholds.");
    case State::Failed:
        return tr("Smart classification was interrupted.");
    }
    return {};
}

QString SmartCategoryController::errorText() const {
    if (state_ != State::Failed)
        return {};
    if (diagnostic_.contains(QStringLiteral("connection refused"), Qt::CaseInsensitive)
        || diagnostic_.contains(QStringLiteral("couldn't connect"), Qt::CaseInsensitive)
        || diagnostic_.contains(QStringLiteral("failed to connect"), Qt::CaseInsensitive)) {
        return tr("Infer Runtime is unavailable. Start it, then try again.");
    }
    return tr("The previous results are still available. Check Infer Runtime, then try again.");
}
QString SmartCategoryController::selectedCategoryId() const {
    return selected_category_id_;
}
QStringList SmartCategoryController::selectedRepresentationKeys() const {
    if (selected_category_id_.isEmpty())
        return {};
    return selected_members_.isEmpty()
               ? QStringList{QStringLiteral("__shadow-empty-smart-category__")}
               : selected_members_;
}

qulonglong SmartCategoryController::uncertainCount() const noexcept {
    return static_cast<qulonglong>(uncertain_members_.size());
}

bool SmartCategoryController::reviewingUncertain() const noexcept {
    return selected_category_id_ == QStringLiteral("__shadow-smart-uncertain__");
}

int SmartCategoryController::uncertaintyRevision() const noexcept {
    return uncertainty_revision_;
}

void SmartCategoryController::ensureCurrent() {
    if (state_ == State::Idle || state_ == State::NeedsUpdate)
        refresh();
}

void SmartCategoryController::refresh() {
    if (busy()) {
        refresh_queued_ = true;
        return;
    }
    startRun(true, false);
}

void SmartCategoryController::resume() {
    if (busy() || !canResume())
        return;
    startRun(false, false);
}

void SmartCategoryController::pause() {
    if (state_ != State::Running)
        return;
    pause_requested_ = true;
    emit stateChanged();
    if (!watcher_.isRunning() && !generation_.isEmpty()) {
        try {
            pause_writer_(generation_);
            restoreSnapshot();
        } catch (const std::exception& error) {
            diagnostic_ = QString::fromUtf8(error.what());
            state_ = State::Failed;
            emit stateChanged();
        }
    }
}

void SmartCategoryController::rebuild() {
    if (busy()) {
        refresh_queued_ = true;
        return;
    }
    startRun(true, true);
}

void SmartCategoryController::selectCategory(const QString& category_id) {
    if (selected_category_id_ == category_id)
        return;
    selected_category_id_ = category_id;
    loadSelectedMembers();
    emit categoriesChanged();
    emit selectionChanged();
}

void SmartCategoryController::selectUncertain() {
    const QString review_id = QStringLiteral("__shadow-smart-uncertain__");
    if (selected_category_id_ == review_id) {
        clearSelection();
        return;
    }
    selected_category_id_ = review_id;
    loadSelectedMembers();
    emit categoriesChanged();
    emit selectionChanged();
}

void SmartCategoryController::clearSelection() {
    if (selected_category_id_.isEmpty())
        return;
    selected_category_id_.clear();
    selected_members_.clear();
    emit categoriesChanged();
    emit selectionChanged();
}

bool SmartCategoryController::isUncertain(
    const QString& photo_id,
    const QString& representation_id
) const {
    return uncertain_members_.contains(representationKey(photo_id, representation_id));
}

QVariantList SmartCategoryController::feedbackCategories(
    const QString& photo_id,
    const QString& representation_id
) const {
    const QString key = representationKey(photo_id, representation_id);
    const QStringList uncertain = uncertainty_categories_by_key_.value(key);
    QVariantList result;
    result.reserve(categories_.size());
    const auto append = [&](const bool uncertain_only) {
        for (const Category& category : categories_) {
            if (!category.enabled || uncertain.contains(category.id) != uncertain_only)
                continue;
            bool matched = false;
            try {
                matched = members_loader_(category.id).contains(key);
            } catch (const std::exception& error) {
                qWarning().noquote()
                    << "Could not load smart-category correction membership:" << error.what();
            }
            result.push_back(
                QVariantMap{
                    {"id", category.id},
                    {"name", category.name},
                    {"uncertain", uncertain_only},
                    {"matched", matched},
                }
            );
        }
    };
    append(true);
    append(false);
    return result;
}

void SmartCategoryController::recordFeedback(
    const QString& photo_id,
    const QString& representation_id,
    const QString& category_id,
    const int decision
) {
    if (photo_id.isEmpty() || representation_id.isEmpty() || category_id.isEmpty() || decision < -1
        || decision > 1) {
        return;
    }
    try {
        feedback_writer_(
            photo_id,
            representation_id,
            category_id,
            static_cast<std::int8_t>(decision)
        );
        restoreSnapshot();
        feedback_refresh_timer_.start();
    } catch (const std::exception& error) {
        diagnostic_ = QString::fromUtf8(error.what());
        state_ = State::Failed;
        emit stateChanged();
    }
}

void SmartCategoryController::updateCategory(
    const QString& id,
    const QString& name,
    const QString& description,
    const double threshold,
    const bool enabled
) {
    const QString clean_name = name.trimmed();
    const QString clean_description = description.trimmed();
    if (clean_name.isEmpty() || clean_description.isEmpty())
        return;
    for (Category& category : categories_) {
        if (category.id == id) {
            category.name = clean_name;
            category.description = clean_description;
            category.minimum_similarity = static_cast<float>(std::clamp(threshold, -1.0, 1.0));
            category.enabled = enabled;
            saveSettings();
            emit categoriesChanged();
            refresh();
            return;
        }
    }
}

void SmartCategoryController::addCategory(const QString& name, const QString& description) {
    const QString clean_name = name.trimmed();
    const QString clean_description = description.trimmed();
    if (clean_name.isEmpty() || clean_description.isEmpty() || categories_.size() >= 32)
        return;
    categories_.push_back({
        generatedCategoryId(clean_name, clean_description),
        clean_name,
        clean_description,
        DEFAULT_MINIMUM_SIMILARITY,
        true,
    });
    saveSettings();
    emit categoriesChanged();
    refresh();
}

void SmartCategoryController::resetDefaults() {
    categories_ = defaultCategories();
    saveSettings();
    emit categoriesChanged();
    refresh();
}
void SmartCategoryController::retranslateUi() {
    emit categoriesChanged();
    emit stateChanged();
}

QString SmartCategoryController::configurationRevision() const {
    QVector<Category> enabled;
    for (const Category& category : categories_) {
        if (category.enabled)
            enabled.push_back(category);
    }
    std::sort(enabled.begin(), enabled.end(), [](const Category& left, const Category& right) {
        return left.id < right.id;
    });
    QByteArray source = QByteArrayLiteral("shadow-smart-categories-v2");
    source.push_back('\0');
    for (const Category& category : enabled) {
        source.append(category.id.toUtf8());
        source.push_back('\0');
        source.append(category.description.trimmed().toUtf8());
        source.push_back('\0');
        source.append(QByteArray::number(category.minimum_similarity, 'g', 9));
        source.push_back('\0');
    }
    return QStringLiteral("smart-config:%1")
        .arg(
            QString::fromLatin1(
                QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex()
            )
        );
}

QVector<BackendSmartCategoryDefinition> SmartCategoryController::enabledDefinitions() const {
    QVector<BackendSmartCategoryDefinition> definitions;
    for (const Category& category : categories_) {
        if (category.enabled) {
            definitions.push_back({
                category.id,
                category.description,
                category.minimum_similarity,
            });
        }
    }
    return definitions;
}

void SmartCategoryController::restoreSnapshot() {
    try {
        const BackendSmartClassificationSnapshot snapshot = snapshot_loader_();
        category_counts_.clear();
        for (const BackendSmartCategoryCount& count : snapshot.category_counts) {
            category_counts_.insert(count.category_id, count.count);
        }
        has_published_results_ = snapshot.has_published_results;
        adaptation_pending_ = snapshot.adaptation_pending;
        generation_ = snapshot.generation;
        config_revision_ = snapshot.config_revision;
        embedding_space_ = snapshot.embedding_space;
        processed_photos_ = snapshot.processed_photos;
        total_photos_ = snapshot.total_photos;
        diagnostic_.clear();
        pause_requested_ = false;

        const QString current_revision = configurationRevision();
        if (snapshot.status.isEmpty() || snapshot.status == QStringLiteral("empty")) {
            state_ = State::NeedsUpdate;
        } else if (snapshot.config_revision != current_revision) {
            state_ = State::NeedsUpdate;
        } else if (snapshot.status == QStringLiteral("complete")) {
            state_ = adaptation_pending_ ? State::NeedsUpdate : State::Ready;
        } else if (snapshot.status == QStringLiteral("failed")) {
            state_ = State::Failed;
        } else {
            // A persisted "running" state means the process stopped between
            // bounded batches. Present it as resumable instead of claiming it
            // is still running after restart.
            state_ = State::Paused;
        }
        loadReviewQueue();
        loadSelectedMembers();
        emit categoriesChanged();
        emit selectionChanged();
        emit stateChanged();
    } catch (const std::exception& error) {
        diagnostic_ = QString::fromUtf8(error.what());
        state_ = State::Failed;
        emit stateChanged();
    }
}

void SmartCategoryController::startRun(const bool start_new, const bool clear_embeddings) {
    if (enabledDefinitions().isEmpty()) {
        diagnostic_ = tr("Enable at least one smart category.");
        state_ = State::Failed;
        emit stateChanged();
        return;
    }
    config_revision_ = configurationRevision();
    if (start_new) {
        generation_.clear();
        processed_photos_ = 0;
        total_photos_ = 0;
    }
    diagnostic_.clear();
    pause_requested_ = false;
    first_batch_ = start_new;
    clear_embeddings_ = clear_embeddings;
    state_ = State::Running;
    emit stateChanged();
    runNextBatch();
}

void SmartCategoryController::runNextBatch() {
    const QVector<BackendSmartCategoryDefinition> definitions = enabledDefinitions();
    const QString config_revision = config_revision_;
    const QString generation = generation_;
    const bool start_new = first_batch_;
    const bool clear_embeddings = clear_embeddings_;
    first_batch_ = false;
    clear_embeddings_ = false;
    watcher_.setFuture(
        QtConcurrent::run([runner = runner_,
                           definitions,
                           config_revision,
                           generation,
                           start_new,
                           clear_embeddings]() {
            SmartCategoryTaskResult result;
            try {
                result.batch =
                    runner(definitions, config_revision, generation, start_new, clear_embeddings);
            } catch (const std::exception& error) {
                result.diagnostic = QString::fromUtf8(error.what());
            }
            return result;
        })
    );
}

void SmartCategoryController::finishBatch() {
    const SmartCategoryTaskResult result = watcher_.result();
    if (!result.diagnostic.isEmpty()) {
        qWarning().noquote() << "Smart category classification failed:" << result.diagnostic;
        diagnostic_ = result.diagnostic;
        state_ = State::Failed;
        emit stateChanged();
        return;
    }

    const BackendSmartClassificationBatch& batch = result.batch;
    generation_ = batch.generation;
    config_revision_ = batch.config_revision;
    embedding_space_ = batch.embedding_space;
    processed_photos_ = batch.processed_photos;
    total_photos_ = batch.total_photos;
    emit stateChanged();

    if (batch.status == QStringLiteral("complete")) {
        restoreSnapshot();
        if (adaptation_pending_)
            feedback_refresh_timer_.stop();
        if (refresh_queued_ || adaptation_pending_) {
            refresh_queued_ = false;
            refresh();
        }
        return;
    }
    if (pause_requested_) {
        try {
            pause_writer_(generation_);
            restoreSnapshot();
        } catch (const std::exception& error) {
            diagnostic_ = QString::fromUtf8(error.what());
            state_ = State::Failed;
            emit stateChanged();
        }
        return;
    }
    runNextBatch();
}

void SmartCategoryController::continuePendingAdaptation() {
    if (!adaptation_pending_ || busy())
        return;
    if (canResume()) {
        resume();
    } else {
        refresh();
    }
}

void SmartCategoryController::loadSelectedMembers() {
    selected_members_.clear();
    if (selected_category_id_.isEmpty() || !has_published_results_)
        return;
    if (reviewingUncertain()) {
        selected_members_.reserve(uncertain_members_.size());
        for (const QString& member : uncertain_members_)
            selected_members_.push_back(member);
        return;
    }
    try {
        selected_members_ = members_loader_(selected_category_id_);
    } catch (const std::exception& error) {
        qWarning().noquote() << "Could not load smart-category members:" << error.what();
    }
}

void SmartCategoryController::loadReviewQueue() {
    uncertain_members_.clear();
    uncertainty_categories_by_key_.clear();
    if (!has_published_results_) {
        ++uncertainty_revision_;
        return;
    }
    try {
        const QVector<BackendSmartCategoryReviewItem> items = review_queue_loader_();
        for (const BackendSmartCategoryReviewItem& item : items) {
            const QString key = representationKey(item.photo_id, item.representation_id);
            uncertain_members_.insert(key);
            QStringList& categories = uncertainty_categories_by_key_[key];
            if (!categories.contains(item.category_id))
                categories.push_back(item.category_id);
        }
    } catch (const std::exception& error) {
        qWarning().noquote() << "Could not load smart-category review queue:" << error.what();
    }
    ++uncertainty_revision_;
    if (reviewingUncertain())
        loadSelectedMembers();
}

QString SmartCategoryController::representationKey(
    const QString& photo_id,
    const QString& representation_id
) {
    return photo_id + QChar{0x001f} + representation_id;
}

void SmartCategoryController::loadSettings() {
    QSettings settings;
    const int settings_version =
        settings.value(QStringLiteral("smartCategories/settingsVersion"), 1).toInt();
    const int count = settings.beginReadArray(QStringLiteral("smartCategories/items"));
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        Category category;
        category.id = settings.value(QStringLiteral("id")).toString();
        category.name = settings.value(QStringLiteral("name")).toString();
        category.description = settings.value(QStringLiteral("description")).toString();
        category.minimum_similarity =
            settings.value(QStringLiteral("minimumSimilarity"), 0.20).toFloat();
        category.enabled = settings.value(QStringLiteral("enabled"), true).toBool();
        if (!category.id.isEmpty() && !category.name.isEmpty() && !category.description.isEmpty()) {
            categories_.push_back(category);
        }
    }
    settings.endArray();
    if (categories_.isEmpty()) {
        categories_ = defaultCategories();
    } else if (settings_version < 2) {
        // The first preview used 0.20, a semantic-search-style absolute
        // threshold that sits above the observed SigLIP shared-space range.
        // Migrate only untouched values; preserve deliberate user tuning.
        for (Category& category : categories_) {
            if (std::abs(category.minimum_similarity - 0.20F) < 0.0001F)
                category.minimum_similarity = DEFAULT_MINIMUM_SIMILARITY;
        }
    }
    if (settings_version < 3) {
        // Competitive scoring still keeps an absolute evidence floor, but
        // SigLIP prompt families have different useful ranges. Migrate only
        // untouched preview defaults; preserve explicit user tuning and every
        // custom category.
        for (Category& category : categories_) {
            const std::optional<float> calibrated = calibratedBuiltInMinimumSimilarity(category.id);
            if (calibrated.has_value()
                && std::abs(category.minimum_similarity - DEFAULT_MINIMUM_SIMILARITY) < 0.0001F) {
                category.minimum_similarity = *calibrated;
            }
        }
    }
    if (settings_version < SMART_CATEGORY_SETTINGS_VERSION) {
        saveSettings();
        settings.setValue(
            QStringLiteral("smartCategories/settingsVersion"),
            SMART_CATEGORY_SETTINGS_VERSION
        );
    }
}

void SmartCategoryController::saveSettings() const {
    QSettings settings;
    settings.beginWriteArray(
        QStringLiteral("smartCategories/items"),
        static_cast<int>(categories_.size())
    );
    for (qsizetype index = 0; index < categories_.size(); ++index) {
        settings.setArrayIndex(static_cast<int>(index));
        const Category& category = categories_.at(index);
        settings.setValue(QStringLiteral("id"), category.id);
        settings.setValue(QStringLiteral("name"), category.name);
        settings.setValue(QStringLiteral("description"), category.description);
        settings.setValue(QStringLiteral("minimumSimilarity"), category.minimum_similarity);
        settings.setValue(QStringLiteral("enabled"), category.enabled);
    }
    settings.endArray();
}

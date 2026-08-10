#pragma once

#include "backend/image_understanding_types.hpp"
#include "backend/smart_category_types.hpp"

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <functional>

struct SmartCategoryTaskResult final {
    BackendSmartClassificationBatch batch;
    QString diagnostic;
};

/// Projects the durable smart-classification job into the desktop UI.
///
/// Rust owns checkpoints and the atomically published membership generation.
/// This controller keeps only category counts plus the currently selected
/// category's member keys, so a large library is not duplicated in Qt memory.
class SmartCategoryController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList categories READ categories NOTIFY categoriesChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canPause READ canPause NOTIFY stateChanged)
    Q_PROPERTY(bool canResume READ canResume NOTIFY stateChanged)
    Q_PROPERTY(bool needsUpdate READ needsUpdate NOTIFY stateChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(bool hasPublishedResults READ hasPublishedResults NOTIFY stateChanged)
    Q_PROPERTY(bool hasMatches READ hasMatches NOTIFY categoriesChanged)
    Q_PROPERTY(qulonglong processedPhotos READ processedPhotos NOTIFY stateChanged)
    Q_PROPERTY(qulonglong totalPhotos READ totalPhotos NOTIFY stateChanged)
    Q_PROPERTY(int progressPercent READ progressPercent NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(QString selectedCategoryId READ selectedCategoryId NOTIFY selectionChanged)
    Q_PROPERTY(qulonglong uncertainCount READ uncertainCount NOTIFY categoriesChanged)
    Q_PROPERTY(bool reviewingUncertain READ reviewingUncertain NOTIFY selectionChanged)
    Q_PROPERTY(int uncertaintyRevision READ uncertaintyRevision NOTIFY categoriesChanged)
    Q_PROPERTY(
        QStringList selectedRepresentationKeys READ selectedRepresentationKeys NOTIFY
            selectionChanged
    )

  public:
    using Runner = std::function<BackendSmartClassificationBatch(
        const QVector<BackendSmartCategoryDefinition>&,
        const QString&,
        const QString&,
        bool,
        bool
    )>;
    using SnapshotLoader = std::function<BackendSmartClassificationSnapshot()>;
    using MembersLoader = std::function<QStringList(const QString&)>;
    using ReviewQueueLoader = std::function<QVector<BackendSmartCategoryReviewItem>()>;
    using FeedbackWriter =
        std::function<void(const QString&, const QString&, const QString&, std::int8_t)>;
    using PauseWriter = std::function<void(const QString&)>;

    explicit SmartCategoryController(
        Runner runner,
        SnapshotLoader snapshot_loader,
        MembersLoader members_loader,
        ReviewQueueLoader review_queue_loader,
        FeedbackWriter feedback_writer,
        PauseWriter pause_writer,
        QObject* parent = nullptr
    );
    ~SmartCategoryController() override;

    [[nodiscard]] QVariantList categories() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool canPause() const noexcept;
    [[nodiscard]] bool canResume() const noexcept;
    [[nodiscard]] bool needsUpdate() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] bool hasPublishedResults() const noexcept;
    [[nodiscard]] bool hasMatches() const noexcept;
    [[nodiscard]] qulonglong processedPhotos() const noexcept;
    [[nodiscard]] qulonglong totalPhotos() const noexcept;
    [[nodiscard]] int progressPercent() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QString selectedCategoryId() const;
    [[nodiscard]] QStringList selectedRepresentationKeys() const;
    [[nodiscard]] qulonglong uncertainCount() const noexcept;
    [[nodiscard]] bool reviewingUncertain() const noexcept;
    [[nodiscard]] int uncertaintyRevision() const noexcept;
    [[nodiscard]] QVector<BackendClassificationReviewCategory> enabledReviewCategories() const;
    [[nodiscard]] QString reviewTaxonomyRevision() const;

    Q_INVOKABLE void ensureCurrent();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void rebuild();
    Q_INVOKABLE void selectCategory(const QString& category_id);
    Q_INVOKABLE void selectUncertain();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool isUncertain(const QString& photo_id, const QString& representation_id) const;
    Q_INVOKABLE QVariantList
    feedbackCategories(const QString& photo_id, const QString& representation_id) const;
    Q_INVOKABLE void recordFeedback(
        const QString& photo_id,
        const QString& representation_id,
        const QString& category_id,
        int decision
    );
    Q_INVOKABLE void updateCategory(
        const QString& category_id,
        const QString& name,
        const QString& description,
        double minimum_similarity,
        bool enabled
    );
    Q_INVOKABLE void addCategory(const QString& name, const QString& description);
    Q_INVOKABLE void resetDefaults();
    Q_INVOKABLE void retranslateUi();
    void reloadAfterExternalFeedback();

  signals:
    void categoriesChanged();
    void stateChanged();
    void selectionChanged();

  private:
    struct Category final {
        QString id;
        QString name;
        QString description;
        float minimum_similarity = 0.20F;
        bool enabled = true;
    };

    enum class State { Idle, Running, Paused, Ready, NeedsUpdate, Failed };

    [[nodiscard]] static QVector<Category> defaultCategories();
    [[nodiscard]] QString configurationRevision() const;
    [[nodiscard]] QVector<BackendSmartCategoryDefinition> enabledDefinitions() const;
    void loadSettings();
    void saveSettings() const;
    void restoreSnapshot();
    void startRun(bool start_new, bool clear_embeddings);
    void runNextBatch();
    void finishBatch();
    void continuePendingAdaptation();
    void loadSelectedMembers();
    void loadReviewQueue();
    [[nodiscard]] static QString
    representationKey(const QString& photo_id, const QString& representation_id);

    Runner runner_;
    SnapshotLoader snapshot_loader_;
    MembersLoader members_loader_;
    ReviewQueueLoader review_queue_loader_;
    FeedbackWriter feedback_writer_;
    PauseWriter pause_writer_;
    QFutureWatcher<SmartCategoryTaskResult> watcher_;
    QVector<Category> categories_;
    QHash<QString, qulonglong> category_counts_;
    QHash<QString, QStringList> uncertainty_categories_by_key_;
    QSet<QString> uncertain_members_;
    QStringList selected_members_;
    QString selected_category_id_;
    QString generation_;
    QString config_revision_;
    QString embedding_space_;
    QString diagnostic_;
    QTimer feedback_refresh_timer_;
    std::uint64_t total_photos_ = 0;
    std::uint64_t processed_photos_ = 0;
    State state_ = State::Idle;
    bool has_published_results_ = false;
    bool pause_requested_ = false;
    bool first_batch_ = false;
    bool clear_embeddings_ = false;
    bool refresh_queued_ = false;
    bool adaptation_pending_ = false;
    int uncertainty_revision_ = 0;
};

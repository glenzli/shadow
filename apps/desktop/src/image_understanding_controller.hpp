#pragma once

#include "ai_preferences.hpp"
#include "backend/image_understanding_types.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QTimer>

#include <functional>

struct ImageUnderstandingTaskResult final {
    BackendImageUnderstandingBatch batch;
    QString diagnostic;
};

struct AdvancedClassificationTaskResult final {
    BackendClassificationReviewProposal proposal;
    QString diagnostic;
};

/// Owns bounded Qwen scheduling and explicit advanced classification review.
///
/// Rust owns durable checkpoints, proposal CAS, and adaptation handoff. This
/// controller owns only desktop admission, yielding between batches, and the
/// currently visible review interaction.
class ImageUnderstandingController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canPause READ canPause NOTIFY stateChanged)
    Q_PROPERTY(bool canResume READ canResume NOTIFY stateChanged)
    Q_PROPERTY(int progressPercent READ progressPercent NOTIFY stateChanged)
    Q_PROPERTY(qulonglong processedPhotos READ processedPhotos NOTIFY stateChanged)
    Q_PROPERTY(qulonglong totalPhotos READ totalPhotos NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(bool advancedReviewBusy READ advancedReviewBusy NOTIFY advancedReviewChanged)
    Q_PROPERTY(QString advancedReviewPhotoId READ advancedReviewPhotoId NOTIFY advancedReviewChanged)
    Q_PROPERTY(
        QString advancedReviewRepresentationId READ advancedReviewRepresentationId NOTIFY
            advancedReviewChanged
    )
    Q_PROPERTY(
        QString advancedReviewDisposition READ advancedReviewDisposition NOTIFY
            advancedReviewChanged
    )
    Q_PROPERTY(
        QString advancedReviewCategoryId READ advancedReviewCategoryId NOTIFY advancedReviewChanged
    )
    Q_PROPERTY(
        QString advancedReviewCategoryName READ advancedReviewCategoryName NOTIFY
            advancedReviewChanged
    )
    Q_PROPERTY(QString advancedReviewError READ advancedReviewError NOTIFY advancedReviewChanged)
    Q_PROPERTY(bool photoProposalAvailable READ photoProposalAvailable NOTIFY photoProposalChanged)
    Q_PROPERTY(QString photoProposalPhotoId READ photoProposalPhotoId NOTIFY photoProposalChanged)
    Q_PROPERTY(
        QString photoProposalRepresentationId READ photoProposalRepresentationId NOTIFY
            photoProposalChanged
    )
    Q_PROPERTY(QString photoDescription READ photoDescription NOTIFY photoProposalChanged)
    Q_PROPERTY(QStringList photoKeywords READ photoKeywords NOTIFY photoProposalChanged)
    Q_PROPERTY(QString photoProposalDisposition READ photoProposalDisposition NOTIFY photoProposalChanged)

  public:
    using BatchRunner = std::function<BackendImageUnderstandingBatch(
        const QString&,
        int,
        const QString&,
        bool,
        bool
    )>;
    using SnapshotLoader = std::function<BackendImageUnderstandingSnapshot()>;
    using PauseWriter = std::function<BackendImageUnderstandingSnapshot(const QString&)>;
    using ReviewRunner = std::function<BackendClassificationReviewProposal(
        const QString&,
        const QString&,
        const QString&,
        const QVector<BackendClassificationReviewCategory>&
    )>;
    using ReviewAccepter =
        std::function<QString(const QString&, const QString&, const QString&)>;
    using ReviewDismisser =
        std::function<void(const QString&, const QString&, const QString&)>;
    using ProposalLoader =
        std::function<BackendImageUnderstandingProposal(const QString&, const QString&)>;
    using KeywordAccepter =
        std::function<void(const QString&, const QString&, const QString&)>;
    using CategoryProvider = std::function<QVector<BackendClassificationReviewCategory>()>;
    using TaxonomyRevisionProvider = std::function<QString()>;

    explicit ImageUnderstandingController(
        AiPreferences* preferences,
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
        QObject* parent = nullptr
    );
    ~ImageUnderstandingController() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool canPause() const noexcept;
    [[nodiscard]] bool canResume() const noexcept;
    [[nodiscard]] int progressPercent() const noexcept;
    [[nodiscard]] qulonglong processedPhotos() const noexcept;
    [[nodiscard]] qulonglong totalPhotos() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] bool advancedReviewBusy() const noexcept;
    [[nodiscard]] QString advancedReviewPhotoId() const;
    [[nodiscard]] QString advancedReviewRepresentationId() const;
    [[nodiscard]] QString advancedReviewDisposition() const;
    [[nodiscard]] QString advancedReviewCategoryId() const;
    [[nodiscard]] QString advancedReviewCategoryName() const;
    [[nodiscard]] QString advancedReviewError() const;
    [[nodiscard]] bool photoProposalAvailable() const noexcept;
    [[nodiscard]] QString photoProposalPhotoId() const;
    [[nodiscard]] QString photoProposalRepresentationId() const;
    [[nodiscard]] QString photoDescription() const;
    [[nodiscard]] QStringList photoKeywords() const;
    [[nodiscard]] QString photoProposalDisposition() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void notifyLibraryChanged();
    Q_INVOKABLE void requestAdvancedReview(
        const QString& photo_id,
        const QString& representation_id
    );
    Q_INVOKABLE void acceptAdvancedReview();
    Q_INVOKABLE void dismissAdvancedReview();
    Q_INVOKABLE void clearAdvancedReview();
    Q_INVOKABLE void loadPhotoProposal(
        const QString& photo_id,
        const QString& representation_id
    );
    Q_INVOKABLE void acceptPhotoKeywords();
    Q_INVOKABLE void retranslateUi();

  signals:
    void stateChanged();
    void advancedReviewChanged();
    void advancedReviewAccepted();
    void photoProposalChanged();

  private:
    enum class State { Disabled, Idle, Running, Paused, Complete, Failed };

    void restoreSnapshot();
    void reconcileBackgroundPolicy();
    void startRun(bool start_new);
    void runNextBatch();
    void finishBatch();
    void finishAdvancedReview();
    [[nodiscard]] QString expectedPolicyRevision() const;
    [[nodiscard]] bool executionAllowed() const noexcept;
    [[nodiscard]] bool backgroundAllowed() const noexcept;
    void setAdvancedProposal(const BackendClassificationReviewProposal& proposal);

    AiPreferences* preferences_ = nullptr;
    BatchRunner batch_runner_;
    SnapshotLoader snapshot_loader_;
    PauseWriter pause_writer_;
    ReviewRunner review_runner_;
    ReviewAccepter review_accepter_;
    ReviewDismisser review_dismisser_;
    ProposalLoader proposal_loader_;
    KeywordAccepter keyword_accepter_;
    CategoryProvider category_provider_;
    TaxonomyRevisionProvider taxonomy_revision_provider_;
    QFutureWatcher<ImageUnderstandingTaskResult> batch_watcher_;
    QFutureWatcher<AdvancedClassificationTaskResult> review_watcher_;
    QTimer next_batch_timer_;
    QString generation_;
    QString policy_revision_;
    QString diagnostic_;
    QString advanced_photo_id_;
    QString advanced_representation_id_;
    QString advanced_source_revision_;
    QString advanced_disposition_;
    QString advanced_category_id_;
    QString advanced_category_name_;
    QString advanced_diagnostic_;
    BackendImageUnderstandingProposal photo_proposal_;
    std::uint64_t processed_photos_ = 0;
    std::uint64_t total_photos_ = 0;
    State state_ = State::Idle;
    bool first_batch_ = false;
    bool pause_requested_ = false;
    bool policy_changed_ = false;
    bool shutting_down_ = false;
};

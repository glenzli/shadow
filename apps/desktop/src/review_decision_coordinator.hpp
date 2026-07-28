#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"
#include "review_decision_session.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>

/// Canonical internal mapping shared by Review page projection and mutation
/// admission.
[[nodiscard]] std::optional<BackendReviewDecisionFlag>
review_decision_flag_from_name(const QString& flag);
[[nodiscard]] QString review_decision_flag_name(BackendReviewDecisionFlag flag);

/// Owns the complete append-only Review flag/rating mutation lifecycle.
///
/// ReviewController decides whether another desktop workflow blocks admission
/// and projects accepted authoritative states into its model. Once admitted,
/// this owner resolves the current state, serializes the backend write,
/// refreshes after failure, validates receipts, maintains causal local undo,
/// publishes localized status, and waits for its worker before destruction.
class ReviewDecisionCoordinator final : public QObject {
    Q_OBJECT

public:
    struct Operations final {
        std::function<BackendReviewDecisionMutationReceipt(
            const QString& photo_id,
            std::uint64_t expected_head_sequence,
            BackendReviewDecisionFlag desired_flag,
            std::uint8_t desired_rating
        )> mutate;
        std::function<BackendReviewDecisionState(const QString& photo_id)>
            authoritative_state;
    };

    using CurrentStateResolver =
        std::function<std::optional<BackendReviewDecisionState>(
            const QString& photo_id
        )>;
    using StateApplied =
        std::function<void(const BackendReviewDecisionState& state)>;

    ReviewDecisionCoordinator(
        Operations operations,
        CurrentStateResolver current_state_resolver,
        StateApplied state_applied,
        QObject* parent = nullptr
    );
    ~ReviewDecisionCoordinator() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] const LocalizedUiMessage& statusMessage() const noexcept;
    [[nodiscard]] QString statusText() const;

    [[nodiscard]] bool setFlag(
        const QString& photo_id,
        const QString& flag,
        bool admitted = true
    );
    [[nodiscard]] bool setRating(
        const QString& photo_id,
        int rating,
        bool admitted = true
    );
    [[nodiscard]] bool undo(bool admitted = true);

    /// Incorporates a Catalog-authoritative state observed by a page refresh
    /// without starting a mutation.
    void reconcile(BackendReviewDecisionState authoritative);
    void retranslateUi();

signals:
    void stateChanged();
    void statusTextChanged();
    void decisionApplied(
        const QString& photoId,
        qulonglong headSequence,
        const QString& flag,
        int rating
    );
    void undone();

private:
    struct TaskResult final {
        BackendReviewDecisionMutationReceipt receipt;
        BackendReviewDecisionState authoritative;
        QString error;
        QString refresh_error;
        bool has_authoritative = false;
        bool is_undo = false;
    };

    [[nodiscard]] static TaskResult runMutation(
        Operations operations,
        ReviewDecisionMutationRequest request
    );
    [[nodiscard]] std::optional<BackendReviewDecisionState> currentState(
        const QString& photo_id,
        const char* missing_message,
        const char* invalid_message
    );
    void startMutation(const ReviewDecisionMutationRequest& request);
    void finishTask();
    void applyState(const BackendReviewDecisionState& state);
    void setStatusMessage(LocalizedUiMessage status);

    Operations operations_;
    CurrentStateResolver current_state_resolver_;
    StateApplied state_applied_;
    ReviewDecisionSession session_;
    LocalizedUiMessage status_message_{
        "ReviewController",
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Flags and stars are explicit local library decisions"
        ),
    };
    QFutureWatcher<TaskResult> watcher_;
};

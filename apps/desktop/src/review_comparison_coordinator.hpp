#pragma once

#include "desktop_backend.hpp"
#include "localized_ui_message.hpp"
#include "review_evidence_session.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <cstdint>
#include <functional>
#include <optional>

/// Owns the complete Review Compare presentation and evidence lifecycle.
///
/// ReviewController remains the public Qt facade and decides whether another
/// desktop workflow currently blocks admission. Once admitted, this owner
/// prepares and verifies the exact frame presentation, serializes record and
/// forget writes, validates their receipts, publishes terminal status, and
/// waits for its worker before destruction.
class ReviewComparisonCoordinator final : public QObject {
    Q_OBJECT

public:
    struct Operations final {
        std::function<BackendReviewComparisonPresentation(
            const QString& left_visual_handle,
            const QString& right_visual_handle
        )> prepare;
        std::function<void(
            const QString& presentation_id,
            const QString& left_request_ticket,
            const QString& right_request_ticket
        )> confirm_ready;
        std::function<void(const QString& presentation_id)> cancel;
        std::function<BackendFeedbackReceipt(
            const QString& presentation_id,
            BackendPairwiseOutcome outcome
        )> record;
        std::function<BackendForgetReceipt(const QString& event_id)> forget;
    };

    using VisualSourceResolver =
        std::function<QString(const QString& request_ticket)>;

    explicit ReviewComparisonCoordinator(
        Operations operations,
        VisualSourceResolver visual_source_resolver,
        QObject* parent = nullptr
    );
    ~ReviewComparisonCoordinator() override;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool canForget() const noexcept;
    [[nodiscard]] int activeCount() const noexcept;
    [[nodiscard]] QString statusText() const;

    [[nodiscard]] QVariantMap prepare(
        const QString& left_visual_handle,
        const QString& right_visual_handle
    );
    [[nodiscard]] bool confirmReady(
        const QString& presentation_id,
        const QString& left_request_ticket,
        const QString& right_request_ticket
    );
    void cancel(const QString& presentation_id);
    [[nodiscard]] bool record(
        const QString& presentation_id,
        int outcome,
        bool admitted = true
    );
    [[nodiscard]] bool forgetLast();
    void retranslateUi();

signals:
    void stateChanged();
    void statusTextChanged();
    void recorded();
    void forgotten();

private:
    enum class TaskKind : std::uint8_t {
        Record,
        Forget,
    };

    struct TaskResult final {
        BackendFeedbackReceipt feedback;
        BackendForgetReceipt forget;
        QString error;
        TaskKind kind = TaskKind::Record;
    };

    [[nodiscard]] static std::optional<BackendPairwiseOutcome> pairwiseOutcome(
        int outcome
    );
    [[nodiscard]] static TaskResult runRecord(
        std::function<BackendFeedbackReceipt(
            const QString& presentation_id,
            BackendPairwiseOutcome outcome
        )> operation,
        QString presentation_id,
        BackendPairwiseOutcome outcome
    );
    [[nodiscard]] static TaskResult runForget(
        std::function<BackendForgetReceipt(const QString& event_id)> operation,
        QString event_id
    );
    void finishTask();
    void setStatusMessage(LocalizedUiMessage status);

    Operations operations_;
    VisualSourceResolver visual_source_resolver_;
    ReviewEvidenceSession session_;
    LocalizedUiMessage status_message_{
        "ReviewController",
        QT_TRANSLATE_NOOP(
            "ReviewController",
            "Explicit choices are recorded as evidence; no preference model is active"
        ),
    };
    QFutureWatcher<TaskResult> watcher_;
};

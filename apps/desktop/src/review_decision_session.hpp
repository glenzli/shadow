#pragma once

#include "desktop_backend.hpp"

#include <QHash>
#include <QString>
#include <QVector>

#include <optional>
#include <utility>

struct ReviewDecisionMutationRequest final {
    QString photo_id;
    std::uint64_t expected_head_sequence = 0;
    BackendReviewDecisionFlag desired_flag = BackendReviewDecisionFlag::Unflagged;
    std::uint8_t desired_rating = 0;
    bool is_undo = false;
};

// UI-thread state for full-state, append-only Review decision mutations. The
// Catalog remains authoritative. This helper validates receipts, serializes
// writes, and keeps only the successful commands issued by this app session.
class ReviewDecisionSession final {
public:
    [[nodiscard]] bool busy() const noexcept {
        return pending_.has_value();
    }

    [[nodiscard]] int undoDepth() const noexcept {
        return static_cast<int>(commands_.size());
    }

    [[nodiscard]] bool canUndo() const {
        if (busy() || commands_.isEmpty()) {
            return false;
        }
        const auto& command = commands_.back();
        const auto current = authoritative_states_.constFind(command.after.photo_id);
        return current != authoritative_states_.cend()
            && same_decision(*current, command.after);
    }

    [[nodiscard]] std::optional<QString> undoPhotoId() const {
        if (commands_.isEmpty()) {
            return std::nullopt;
        }
        return commands_.back().after.photo_id;
    }

    [[nodiscard]] std::optional<ReviewDecisionMutationRequest> beginSet(
        BackendReviewDecisionState current,
        const BackendReviewDecisionFlag desired_flag,
        const std::uint8_t desired_rating
    ) {
        if (busy() || current.photo_id.trimmed().isEmpty() || desired_rating > 5
            || (current.flag == desired_flag && current.rating == desired_rating)) {
            return std::nullopt;
        }
        authoritative_states_.insert(current.photo_id, current);
        pending_ = PendingMutation{
            .request = {
                .photo_id = current.photo_id,
                .expected_head_sequence = current.head_sequence,
                .desired_flag = desired_flag,
                .desired_rating = desired_rating,
                .is_undo = false,
            },
            .expected_before = std::move(current),
            .undo_target = std::nullopt,
        };
        return pending_->request;
    }

    [[nodiscard]] std::optional<ReviewDecisionMutationRequest> beginUndo() {
        if (!canUndo()) {
            return std::nullopt;
        }
        const auto& command = commands_.back();
        const auto current = authoritative_states_.constFind(command.after.photo_id);
        pending_ = PendingMutation{
            .request = {
                .photo_id = current->photo_id,
                .expected_head_sequence = current->head_sequence,
                .desired_flag = command.before.flag,
                .desired_rating = command.before.rating,
                .is_undo = true,
            },
            .expected_before = *current,
            .undo_target = command,
        };
        return pending_->request;
    }

    [[nodiscard]] bool complete(
        const BackendReviewDecisionMutationReceipt& receipt
    ) {
        if (!valid_receipt(receipt)) {
            fail();
            return false;
        }
        const PendingMutation pending = *pending_;
        authoritative_states_.insert(receipt.after.photo_id, receipt.after);
        if (pending.request.is_undo) {
            const Command undone = *pending.undo_target;
            commands_.pop_back();
            rebindCausalPredecessor(undone, receipt.after);
        } else {
            commands_.push_back({.before = receipt.before, .after = receipt.after});
        }
        pending_.reset();
        return true;
    }

    void reconcile(BackendReviewDecisionState authoritative) {
        if (authoritative.photo_id.trimmed().isEmpty() || authoritative.rating > 5) {
            return;
        }
        authoritative_states_.insert(authoritative.photo_id, std::move(authoritative));
    }

    void fail() noexcept {
        pending_.reset();
    }

private:
    struct Command final {
        BackendReviewDecisionState before;
        BackendReviewDecisionState after;
    };

    struct PendingMutation final {
        ReviewDecisionMutationRequest request;
        BackendReviewDecisionState expected_before;
        std::optional<Command> undo_target;
    };

    [[nodiscard]] static bool same_decision(
        const BackendReviewDecisionState& left,
        const BackendReviewDecisionState& right
    ) {
        return left == right;
    }

    // A verified inverse event creates a new head for the state that existed
    // immediately before the undone command. If that exact old head was itself
    // produced by an earlier local command, advance only that command's trusted
    // after-state to the inverse event. This preserves chained local undo while
    // refusing to bridge an intervening external history that merely returns to
    // the same flag/rating values at a different head.
    void rebindCausalPredecessor(
        const Command& undone,
        const BackendReviewDecisionState& restored
    ) {
        for (qsizetype index = commands_.size(); index > 0; --index) {
            auto& candidate = commands_[index - 1];
            if (candidate.after.photo_id != restored.photo_id) {
                continue;
            }
            if (candidate.after == undone.before) {
                candidate.after = restored;
            }
            return;
        }
    }

    [[nodiscard]] bool valid_receipt(
        const BackendReviewDecisionMutationReceipt& receipt
    ) const {
        if (!pending_ || receipt.event_id.trimmed().isEmpty()
            || receipt.sequence == 0 || receipt.occurred_at_ms <= 0
            || receipt.sequence != receipt.after.head_sequence
            || receipt.before != pending_->expected_before
            || receipt.after.photo_id != pending_->request.photo_id
            || receipt.after.flag != pending_->request.desired_flag
            || receipt.after.rating != pending_->request.desired_rating
            || receipt.after.rating > 5
            || receipt.sequence <= receipt.before.head_sequence) {
            return false;
        }
        if (!pending_->request.is_undo) {
            return !pending_->undo_target.has_value();
        }
        if (!pending_->undo_target || commands_.isEmpty()) {
            return false;
        }
        const auto& current_target = commands_.back();
        return pending_->undo_target->before == current_target.before
            && pending_->undo_target->after == current_target.after
            && receipt.after.flag == current_target.before.flag
            && receipt.after.rating == current_target.before.rating;
    }

    QHash<QString, BackendReviewDecisionState> authoritative_states_;
    QVector<Command> commands_;
    std::optional<PendingMutation> pending_;
};

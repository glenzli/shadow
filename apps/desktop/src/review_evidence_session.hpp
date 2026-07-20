#pragma once

#include <QString>
#include <QVector>

#include <optional>
#include <utility>

// UI-thread state for asynchronous, append-only Review evidence writes. The
// durable Catalog remains authoritative; this helper only prevents duplicate
// in-flight actions and tracks which events this desktop session may forget.
class ReviewEvidenceSession final {
public:
    [[nodiscard]] bool busy() const noexcept {
        return pending_ != Pending::None;
    }

    [[nodiscard]] bool canForget() const noexcept {
        return !busy() && !active_event_ids_.isEmpty();
    }

    [[nodiscard]] int activeCount() const noexcept {
        return static_cast<int>(active_event_ids_.size());
    }

    [[nodiscard]] bool beginRecord() noexcept {
        if (busy()) {
            return false;
        }
        pending_ = Pending::Record;
        return true;
    }

    [[nodiscard]] std::optional<QString> beginForget() {
        if (!canForget()) {
            return std::nullopt;
        }
        pending_ = Pending::Forget;
        pending_forget_target_ = active_event_ids_.back();
        return pending_forget_target_;
    }

    [[nodiscard]] bool completeRecord(QString event_id) {
        if (pending_ != Pending::Record || event_id.trimmed().isEmpty()) {
            fail();
            return false;
        }
        active_event_ids_.push_back(std::move(event_id));
        clearPending();
        return true;
    }

    [[nodiscard]] bool completeForget(const QString& target_event_id) {
        if (pending_ != Pending::Forget || active_event_ids_.isEmpty()
            || target_event_id != pending_forget_target_
            || active_event_ids_.back() != target_event_id) {
            fail();
            return false;
        }
        active_event_ids_.pop_back();
        clearPending();
        return true;
    }

    void fail() noexcept {
        clearPending();
    }

private:
    enum class Pending {
        None,
        Record,
        Forget,
    };

    void clearPending() noexcept {
        pending_ = Pending::None;
        pending_forget_target_.clear();
    }

    QVector<QString> active_event_ids_;
    QString pending_forget_target_;
    Pending pending_ = Pending::None;
};

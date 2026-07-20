#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A bounded, session-only history for immutable value snapshots. The caller owns
// persistence and preview rendering; this helper only records working-state
// transitions. A gesture remains pending until it ends, so an arbitrary number
// of slider updates becomes exactly one undo entry.
template <typename State, std::size_t MaxEntries = 256>
class SessionEditHistory final {
    static_assert(MaxEntries > 0);

public:
    [[nodiscard]] bool canUndo() const noexcept {
        return !undo_.empty() || pendingChanged();
    }

    [[nodiscard]] bool canRedo() const noexcept {
        return !pendingChanged() && !redo_.empty();
    }

    [[nodiscard]] std::size_t undoDepth() const noexcept {
        return undo_.size() + (pendingChanged() ? 1U : 0U);
    }

    [[nodiscard]] std::size_t redoDepth() const noexcept {
        return canRedo() ? redo_.size() : 0U;
    }

    void clear() noexcept {
        undo_.clear();
        redo_.clear();
        pending_.reset();
    }

    void beginGesture(std::string key, const State& current) {
        if (pending_ && pending_->key == key) {
            return;
        }
        finishGesture(current);
        pending_ = Pending{std::move(key), current, current};
    }

    void record(
        std::string key,
        const State& before,
        const State& after
    ) {
        if (before == after) {
            return;
        }
        if (pending_ && pending_->key == key && pending_->after == before) {
            pending_->after = after;
            return;
        }
        if (pending_) {
            finishGesture(before);
        }
        pushUndo(Entry{std::move(before), after, std::move(key)});
        redo_.clear();
    }

    void endGesture(const std::string_view key, const State& current) {
        if (!pending_ || pending_->key != key) {
            return;
        }
        finishGesture(current);
    }

    void finishGesture(const State& current) {
        if (!pending_) {
            return;
        }
        pending_->after = current;
        if (pending_->before != pending_->after) {
            pushUndo(Entry{
                std::move(pending_->before),
                std::move(pending_->after),
                std::move(pending_->key),
            });
            redo_.clear();
        }
        pending_.reset();
    }

    [[nodiscard]] std::optional<State> undo(
        const State& current,
        std::string* const restored_key = nullptr
    ) {
        if (restored_key != nullptr) {
            restored_key->clear();
        }
        finishGesture(current);
        if (undo_.empty()) {
            return std::nullopt;
        }
        Entry entry = std::move(undo_.back());
        undo_.pop_back();
        if (entry.after != current) {
            clear();
            return std::nullopt;
        }
        const State result = entry.before;
        if (restored_key != nullptr) {
            *restored_key = entry.key;
        }
        redo_.push_back(std::move(entry));
        return result;
    }

    [[nodiscard]] std::optional<State> redo(
        const State& current,
        std::string* const restored_key = nullptr
    ) {
        if (restored_key != nullptr) {
            restored_key->clear();
        }
        finishGesture(current);
        if (redo_.empty()) {
            return std::nullopt;
        }
        Entry entry = std::move(redo_.back());
        redo_.pop_back();
        if (entry.before != current) {
            clear();
            return std::nullopt;
        }
        const State result = entry.after;
        if (restored_key != nullptr) {
            *restored_key = entry.key;
        }
        pushUndo(std::move(entry));
        return result;
    }

private:
    struct Entry final {
        State before;
        State after;
        std::string key;
    };

    struct Pending final {
        std::string key;
        State before;
        State after;
    };

    [[nodiscard]] bool pendingChanged() const noexcept {
        return pending_ && pending_->before != pending_->after;
    }

    void pushUndo(Entry entry) {
        if (undo_.size() == MaxEntries) {
            undo_.erase(undo_.begin());
        }
        undo_.push_back(std::move(entry));
    }

    std::vector<Entry> undo_;
    std::vector<Entry> redo_;
    std::optional<Pending> pending_;
};

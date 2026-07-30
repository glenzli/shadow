#include "library_keyword_fixture.hpp"

#include <future>

namespace review_library_keyword_test {

void run_failure_lifetime_contracts() {
    {
        auto state = std::make_shared<KeywordBackendState>();
        state->keywords = {
            keyword(QStringLiteral("existing"), QStringLiteral("Existing")),
        };
        state->fail_create = true;
        ReviewLibraryKeywordCoordinator coordinator(operations(state));
        coordinator.createKeyword({}, QStringLiteral("Broken"));
        wait_until([&coordinator]() { return !coordinator.busy(); }, "failed creation settles");
        require(
            coordinator.keywords().size() == 1
                && coordinator.statusMessage().translated().contains(
                    QStringLiteral("create failed")
                ),
            "a failed mutation restores the authoritative projection and diagnostics"
        );
    }

    {
        auto state = std::make_shared<KeywordBackendState>();
        state->keywords = {
            keyword(QStringLiteral("root"), QStringLiteral("Root")),
        };
        state->assignments.insert(QStringLiteral("photo-b"), {assignment(state->keywords.at(0))});
        state->block_tree = true;
        ReviewLibraryKeywordCoordinator coordinator(operations(state));
        coordinator.setPhotoId(QStringLiteral("photo-a"));
        {
            std::unique_lock lock(state->mutex);
            state->condition.wait(lock, [state]() { return state->tree_entered; });
        }
        coordinator.setPhotoId(QStringLiteral("photo-b"));
        {
            std::lock_guard lock(state->mutex);
            state->release_tree = true;
            state->block_tree = false;
        }
        state->condition.notify_all();
        wait_until(
            [&coordinator]() { return !coordinator.busy(); },
            "stale selection refresh is replaced"
        );
        require(
            coordinator.photoId() == QStringLiteral("photo-b")
                && coordinator.photoKeywords().size() == 1,
            "a stale completion cannot publish the prior photo assignments"
        );
    }

    {
        auto state = std::make_shared<KeywordBackendState>();
        state->keywords = {
            keyword(QStringLiteral("root"), QStringLiteral("Root")),
        };
        ReviewLibraryKeywordCoordinator coordinator(operations(state));
        coordinator.setPhotoId(QStringLiteral("photo-a"));
        wait_until(
            [&coordinator]() { return !coordinator.busy(); },
            "initial selected-photo refresh completes"
        );
        int accepted_mutations = 0;
        QObject::connect(
            &coordinator,
            &ReviewLibraryKeywordCoordinator::keywordMutationAccepted,
            [&accepted_mutations]() { ++accepted_mutations; }
        );
        {
            std::lock_guard lock(state->mutex);
            state->block_tree = true;
            state->tree_entered = false;
            state->release_tree = false;
        }
        coordinator.assignKeyword(QStringLiteral("root"), {target(QStringLiteral("photo-a"))});
        {
            std::unique_lock lock(state->mutex);
            state->condition.wait(lock, [state]() { return state->tree_entered; });
        }
        coordinator.setPhotoId(QStringLiteral("photo-b"));
        {
            std::lock_guard lock(state->mutex);
            state->release_tree = true;
            state->block_tree = false;
        }
        state->condition.notify_all();
        wait_until(
            [&coordinator]() { return !coordinator.busy(); },
            "mutation and replacement selection refresh settle"
        );
        require(
            accepted_mutations == 1 && coordinator.photoId() == QStringLiteral("photo-b")
                && coordinator.photoKeywords().isEmpty(),
            "a durable mutation still invalidates consumers when its selected-photo projection "
            "becomes stale"
        );
    }

    {
        auto state = std::make_shared<KeywordBackendState>();
        state->block_tree = true;
        auto coordinator = std::make_unique<ReviewLibraryKeywordCoordinator>(operations(state));
        coordinator->refresh();
        {
            std::unique_lock lock(state->mutex);
            state->condition.wait(lock, [state]() { return state->tree_entered; });
        }
        auto destruction =
            std::async(std::launch::async, [&coordinator]() { coordinator.reset(); });
        require(
            destruction.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout,
            "destruction waits for the active worker"
        );
        {
            std::lock_guard lock(state->mutex);
            state->release_tree = true;
        }
        state->condition.notify_all();
        require(
            destruction.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "destruction completes after the worker releases"
        );
    }
}

} // namespace review_library_keyword_test

#include "edit_preview_presentation_context.hpp"

#include <QCoreApplication>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

class EditPreviewPresentationContextTestAccess final {
  public:
    static void publish(
        EditPreviewPresentationContext& context,
        const std::uintptr_t window_identity,
        const EditPreviewPresentationApi api,
        const std::uintptr_t device_handle,
        const bool initialized
    ) {
        context.publishObservation(window_identity, api, device_handle, initialized);
    }
};

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "edit preview presentation context failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] bool is_first_live_tuple(const EditPreviewPresentationBinding& value) noexcept {
    return value.window_identity == 101U && value.api == EditPreviewPresentationApi::Metal
           && value.device_handle == 202U && value.initialized;
}

[[nodiscard]] bool is_second_live_tuple(const EditPreviewPresentationBinding& value) noexcept {
    return value.window_identity == 303U && value.api == EditPreviewPresentationApi::Software
           && value.device_handle == 0U && value.initialized;
}

void publication_is_atomic_and_epoch_guarded() {
    EditPreviewPresentationContext context;
    const auto initial = context.snapshot();
    require(
        initial.epoch == 0U && initial.window_identity == 0U
            && initial.api == EditPreviewPresentationApi::Unavailable && initial.device_handle == 0U
            && !initial.initialized,
        "the context must start as one unavailable tuple"
    );

    EditPreviewPresentationContextTestAccess::publish(
        context,
        101U,
        EditPreviewPresentationApi::Metal,
        202U,
        true
    );
    const auto first = context.snapshot();
    require(
        is_first_live_tuple(first) && first.epoch == 1U,
        "the first live scene graph must advance one epoch"
    );
    EditPreviewPresentationContextTestAccess::publish(
        context,
        101U,
        EditPreviewPresentationApi::Metal,
        202U,
        true
    );
    require(
        context.snapshot().epoch == first.epoch,
        "re-observing the same scene graph tuple must be idempotent"
    );

    EditPreviewPresentationContextTestAccess::publish(
        context,
        101U,
        EditPreviewPresentationApi::Unavailable,
        0U,
        false
    );
    const auto invalidated = context.snapshot();
    require(
        invalidated.epoch == first.epoch + 1U && invalidated.window_identity == 101U
            && invalidated.api == EditPreviewPresentationApi::Unavailable
            && invalidated.device_handle == 0U && !invalidated.initialized,
        "scene graph invalidation must publish a new unavailable epoch"
    );

    EditPreviewPresentationContextTestAccess::publish(
        context,
        101U,
        EditPreviewPresentationApi::Metal,
        202U,
        true
    );
    std::atomic_bool start{false};
    std::atomic_bool valid{true};
    std::vector<std::thread> readers;
    readers.reserve(6U);
    for (std::size_t index = 0U; index < 6U; ++index) {
        readers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t sample = 0U; sample < 20'000U; ++sample) {
                const auto observed = context.snapshot();
                if (!is_first_live_tuple(observed) && !is_second_live_tuple(observed)) {
                    valid.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (std::size_t sample = 0U; sample < 20'000U; ++sample) {
        if (sample % 2U == 0U) {
            EditPreviewPresentationContextTestAccess::publish(
                context,
                303U,
                EditPreviewPresentationApi::Software,
                0U,
                true
            );
        } else {
            EditPreviewPresentationContextTestAccess::publish(
                context,
                101U,
                EditPreviewPresentationApi::Metal,
                202U,
                true
            );
        }
    }
    for (auto& reader : readers) {
        reader.join();
    }
    require(
        valid.load(std::memory_order_relaxed),
        "readers must never observe a tuple assembled from two epochs"
    );
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    publication_is_atomic_and_epoch_guarded();
    return EXIT_SUCCESS;
}

#include "decoder/decode_session_isolation.hpp"

#include <shadow/image/decoder.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>

namespace image = shadow::image;
namespace isolation = shadow::image::detail;

namespace {

using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

struct ConcurrencyProbe final {
    void enter() {
        const int concurrent = active.fetch_add(1, std::memory_order_acq_rel) + 1;
        int observed = maximum.load(std::memory_order_relaxed);
        while (
            observed < concurrent
            && !maximum.compare_exchange_weak(
                observed,
                concurrent,
                std::memory_order_release,
                std::memory_order_relaxed
            )
        ) {
        }

        entries.fetch_add(1, std::memory_order_release);
        wait_condition.notify_all();
        {
            std::unique_lock lock(wait_mutex);
            static_cast<void>(wait_condition.wait_for(lock, 100ms, [this] {
                return entries.load(std::memory_order_acquire) >= 2;
            }));
        }
        active.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    std::atomic<int> entries{0};
    std::mutex wait_mutex;
    std::condition_variable wait_condition;
};

class ProbeSession final : public image::DecodeSession {
public:
    explicit ProbeSession(std::shared_ptr<ConcurrencyProbe> probe)
        : probe_(std::move(probe)) {
        metadata_.image_dimensions = {1U, 1U};
        capabilities_.metadata = true;
        capabilities_.reference_rgb = true;
    }

    [[nodiscard]] const image::AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] const image::DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::span<const image::PreviewDescriptor> previews() const noexcept override {
        return {};
    }

    [[nodiscard]] image::PreviewPayload decode_preview(std::size_t) override {
        probe_->enter();
        return {};
    }

    [[nodiscard]] image::RawFrame decode_raw_frame() override {
        probe_->enter();
        return {};
    }

    [[nodiscard]] image::PixelBuffer render_reference_rgb() const override {
        probe_->enter();
        image::PixelBuffer result;
        result.dimensions = {1U, 1U};
        result.bits_per_channel = 16U;
        result.channels = 3U;
        result.row_stride_bytes = 3U * sizeof(std::uint16_t);
        result.primaries = image::RgbPrimaries::srgb_rec709_d65;
        result.transfer_function = image::RgbTransferFunction::linear;
        result.reference = image::RgbBufferReference::processed_raw;
        result.samples = {1U, 2U, 3U};
        return result;
    }

private:
    std::shared_ptr<ConcurrencyProbe> probe_;
    image::AssetMetadata metadata_;
    image::DecodeCapabilities capabilities_;
};

void render_together(image::DecodeSession& first, image::DecodeSession& second) {
    std::barrier start(2);
    auto first_render = std::async(std::launch::async, [&first, &start] {
        start.arrive_and_wait();
        return first.render_reference_rgb();
    });
    auto second_render = std::async(std::launch::async, [&second, &start] {
        start.arrive_and_wait();
        return second.render_reference_rgb();
    });
    static_cast<void>(first_render.get());
    static_cast<void>(second_render.get());
}

void one_session_is_always_fail_safe() {
    auto probe = std::make_shared<ConcurrencyProbe>();
    auto session = isolation::isolate_decode_session(
        std::make_unique<ProbeSession>(probe)
    );

    render_together(*session, *session);
    expect(
        probe->maximum.load(std::memory_order_acquire) == 1,
        "one decode session is never entered concurrently"
    );
    expect(
        !isolation::decode_session_uses_shared_provider_gate(*session),
        "a reentrant public-provider session has no process-wide provider gate"
    );
}

void independent_public_sessions_can_run_in_parallel() {
    auto probe = std::make_shared<ConcurrencyProbe>();
    auto first = isolation::isolate_decode_session(std::make_unique<ProbeSession>(probe));
    auto second = isolation::isolate_decode_session(std::make_unique<ProbeSession>(probe));

    render_together(*first, *second);
    expect(
        probe->maximum.load(std::memory_order_acquire) == 2,
        "independent public LibRaw/raster-style sessions can overlap"
    );
}

void one_private_module_is_serialized_across_router_lifetimes() {
    auto probe = std::make_shared<ConcurrencyProbe>();
    auto first_gate = isolation::shared_decode_provider_gate(
        "contract.private-provider.same-module"
    );
    auto second_gate = isolation::shared_decode_provider_gate(
        "contract.private-provider.same-module"
    );
    auto first = isolation::isolate_decode_session(
        std::make_unique<ProbeSession>(probe),
        first_gate
    );
    auto second = isolation::isolate_decode_session(
        std::make_unique<ProbeSession>(probe),
        second_gate
    );

    render_together(*first, *second);
    expect(
        probe->maximum.load(std::memory_order_acquire) == 1,
        "sessions from the same private module share one provider gate"
    );
    expect(
        isolation::decode_session_uses_shared_provider_gate(*first)
            && isolation::decode_session_uses_shared_provider_gate(*second),
        "private sessions retain their cache-independent isolation domain"
    );
}

void unrelated_private_modules_do_not_block_each_other() {
    auto probe = std::make_shared<ConcurrencyProbe>();
    auto first = isolation::isolate_decode_session(
        std::make_unique<ProbeSession>(probe),
        isolation::shared_decode_provider_gate("contract.private-provider.module-a")
    );
    auto second = isolation::isolate_decode_session(
        std::make_unique<ProbeSession>(probe),
        isolation::shared_decode_provider_gate("contract.private-provider.module-b")
    );

    render_together(*first, *second);
    expect(
        probe->maximum.load(std::memory_order_acquire) == 2,
        "unrelated private provider modules retain independent concurrency domains"
    );
}

void router_assigns_only_private_modules_a_shared_provider_gate() {
#if defined(SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH) && defined(SHADOW_TEST_JPEG_PATH)
    const auto router = image::make_photo_decoder_provider(
        SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH
    );
    const auto private_session = router->open("fixture-private-provider.raw");
    auto raster_session = router->open(SHADOW_TEST_JPEG_PATH);
    auto second_raster_session = router->open(SHADOW_TEST_JPEG_PATH);
    expect(
        isolation::decode_session_uses_shared_provider_gate(*private_session),
        "router assigns a module gate to a private provider session"
    );
    expect(
        !isolation::decode_session_uses_shared_provider_gate(*raster_session)
            && !isolation::decode_session_uses_shared_provider_gate(*second_raster_session),
        "router leaves an independent raster session outside private module gates"
    );
    render_together(*raster_session, *second_raster_session);
#else
    expect(false, "router isolation fixtures must be configured");
#endif
}

void configured_libraw_sessions_smoke_in_parallel() {
    const char* fixture = std::getenv("SHADOW_TEST_DNG");
    if (fixture == nullptr || *fixture == '\0') {
        return;
    }

    const auto router = image::make_photo_decoder_provider(std::filesystem::path{});
    auto first = router->open(fixture);
    auto second = router->open(fixture);
    expect(
        !isolation::decode_session_uses_shared_provider_gate(*first)
            && !isolation::decode_session_uses_shared_provider_gate(*second),
        "actual routed libraw_r sessions use only their independent session gates"
    );
    render_together(*first, *second);
}

} // namespace

int main() {
    one_session_is_always_fail_safe();
    independent_public_sessions_can_run_in_parallel();
    one_private_module_is_serialized_across_router_lifetimes();
    unrelated_private_modules_do_not_block_each_other();
    router_assigns_only_private_modules_a_shared_provider_gate();
    configured_libraw_sessions_smoke_in_parallel();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "edit_preview_contract.hpp"
#include "edit_preview_provider.hpp"

#include <QBuffer>
#include <QColor>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QImage>
#include <QQuickTextureFactory>

#include <algorithm>
#include <atomic>
#include <cstdint>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "edit preview contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] QByteArray encoded_square(const QColor color) {
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly), "test image buffer must open");
    require(image.save(&buffer, "PNG"), "test image must encode");
    return bytes;
}

[[nodiscard]] QByteArray rgb_square(const QColor color) {
    QByteArray bytes(2 * 2 * 3, Qt::Uninitialized);
    for (qsizetype index = 0; index < bytes.size(); index += 3) {
        bytes[index] = static_cast<char>(color.red());
        bytes[index + 1] = static_cast<char>(color.green());
        bytes[index + 2] = static_cast<char>(color.blue());
    }
    return bytes;
}

class TestOwnedPreviewFrame final : public BackendEditPreviewFrame {
  public:
    TestOwnedPreviewFrame(
        const QColor color,
        const bool include_coverage,
        std::shared_ptr<std::atomic_uint32_t> destructions
    ) : rgb8_(12U), coverage_(include_coverage ? 4U : 0U), destructions_(std::move(destructions)) {
        for (std::size_t index = 0; index < rgb8_.size(); index += 3U) {
            rgb8_[index] = static_cast<std::uint8_t>(color.red());
            rgb8_[index + 1U] = static_cast<std::uint8_t>(color.green());
            rgb8_[index + 2U] = static_cast<std::uint8_t>(color.blue());
        }
        if (include_coverage) {
            coverage_ = {0U, 64U, 128U, 255U};
        }
    }

    ~TestOwnedPreviewFrame() override {
        destructions_->fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] QSize dimensions() const noexcept override {
        return {2, 2};
    }

    [[nodiscard]] std::size_t rowStrideBytes() const noexcept override {
        return 6U;
    }

    [[nodiscard]] BackendEditPreviewStorage storageKind() const noexcept override {
        return BackendEditPreviewStorage::HostRgb8;
    }

    [[nodiscard]] std::optional<BackendAppleMetalPreviewTexture>
    appleMetalTexture() const noexcept override {
        return std::nullopt;
    }

    [[nodiscard]] std::size_t materializedPixelBytes() const noexcept override {
        return rgb8_.size();
    }

    [[nodiscard]] std::span<const std::uint8_t> materializeRgb8() const override {
        materialization_count_.fetch_add(1U, std::memory_order_relaxed);
        return rgb8_;
    }

    [[nodiscard]] std::optional<BackendEditMaskCoverageView>
    maskCoverage() const noexcept override {
        if (coverage_.empty()) {
            return std::nullopt;
        }
        return BackendEditMaskCoverageView{
            .samples = coverage_,
            .version = EDIT_MASK_COVERAGE_VERSION,
            .target_layer_index = 2U,
            .selection_revision = 5U,
            .dimensions = {2, 2},
            .row_stride_bytes = 2U,
        };
    }

    [[nodiscard]] std::uint64_t retainedBytes() const noexcept override {
        return static_cast<std::uint64_t>(rgb8_.size() + coverage_.size());
    }

    [[nodiscard]] std::string presentationFallbackDiagnostic() const override {
        return {};
    }

    [[nodiscard]] std::uint32_t materializationCount() const noexcept {
        return materialization_count_.load(std::memory_order_relaxed);
    }

  private:
    std::vector<std::uint8_t> rgb8_;
    std::vector<std::uint8_t> coverage_;
    std::shared_ptr<std::atomic_uint32_t> destructions_;
    mutable std::atomic_uint32_t materialization_count_{0U};
};

class OversizedDescriptorFrame final : public BackendEditPreviewFrame {
  public:
    [[nodiscard]] QSize dimensions() const noexcept override {
        return {
            std::numeric_limits<int>::max(),
            std::numeric_limits<int>::max(),
        };
    }

    [[nodiscard]] std::size_t rowStrideBytes() const noexcept override {
        return static_cast<std::size_t>(std::numeric_limits<int>::max()) * 3U;
    }

    [[nodiscard]] BackendEditPreviewStorage storageKind() const noexcept override {
        return BackendEditPreviewStorage::HostRgb8;
    }

    [[nodiscard]] std::optional<BackendAppleMetalPreviewTexture>
    appleMetalTexture() const noexcept override {
        return std::nullopt;
    }

    [[nodiscard]] std::size_t materializedPixelBytes() const noexcept override {
        return 0U;
    }

    [[nodiscard]] std::span<const std::uint8_t> materializeRgb8() const override {
        return {};
    }

    [[nodiscard]] std::optional<BackendEditMaskCoverageView>
    maskCoverage() const noexcept override {
        return BackendEditMaskCoverageView{
            .version = EDIT_MASK_COVERAGE_VERSION,
            .target_layer_index = 2U,
            .selection_revision = 5U,
            .dimensions = dimensions(),
            .row_stride_bytes = static_cast<std::size_t>(std::numeric_limits<int>::max()),
        };
    }

    [[nodiscard]] std::uint64_t retainedBytes() const noexcept override {
        return 0U;
    }

    [[nodiscard]] std::string presentationFallbackDiagnostic() const override {
        return {};
    }
};

[[nodiscard]] EditMaskCoveragePayload owner_coverage_descriptor() {
    return {
        .dimensions = QSize(2, 2),
        .row_stride_bytes = 2U,
        .version = EDIT_MASK_COVERAGE_VERSION,
        .target_layer_index = 2U,
        .selection_revision = 5U,
    };
}

[[nodiscard]] QString mask_provider_request(const MaskCoverageGeneration generation) {
    return QStringLiteral(
               "scope/mask/current?photo=%1&recipe=%2&target=%3"
               "&selection=%4&preview=%5"
    )
        .arg(generation.photo)
        .arg(generation.recipe_revision)
        .arg(generation.target_layer_index)
        .arg(generation.selection_revision)
        .arg(generation.paired_preview_generation);
}

void slots_have_independent_generations() {
    EditPreviewStore store;
    store.publish(EditPreviewSlot::Current, QByteArrayLiteral("current"), QSize(20, 10), 0, {}, 7);
    store.publish(EditPreviewSlot::Before, QByteArrayLiteral("before"), QSize(30, 15), 0, {}, 3);

    require(
        store.snapshot(EditPreviewSlot::Current, 7).bytes == QByteArrayLiteral("current"),
        "current bytes must be read only with the current generation"
    );
    require(
        store.snapshot(EditPreviewSlot::Before, 3).bytes == QByteArrayLiteral("before"),
        "before bytes must be read only with the before generation"
    );
    require(
        store.snapshot(EditPreviewSlot::Current, 3).bytes.isEmpty(),
        "a before generation must not address the current slot"
    );
    require(
        store.snapshot(EditPreviewSlot::Before, 7).bytes.isEmpty(),
        "a current generation must not address the before slot"
    );

    store.clearAll(8, 4);
    require(
        store.snapshot(EditPreviewSlot::Current, 7).bytes.isEmpty()
            && store.snapshot(EditPreviewSlot::Before, 3).bytes.isEmpty(),
        "changing photos must invalidate both old slot generations"
    );
}

void provider_routes_only_named_slots() {
    auto store = std::make_shared<EditPreviewStore>();
    store->publish(EditPreviewSlot::Current, encoded_square(Qt::red), QSize(2, 2), 0, {}, 10);
    store->publish(EditPreviewSlot::Before, encoded_square(Qt::blue), QSize(2, 2), 0, {}, 20);
    EditPreviewProvider provider(store);

    QSize decoded_size;
    const QImage current = provider.requestImage(
        QStringLiteral("current?generation=10"),
        &decoded_size,
        {}
    );
    require(!current.isNull() && current.pixelColor(0, 0) == QColor(Qt::red),
            "the current URL must decode the current slot");
    const QImage before = provider.requestImage(
        QStringLiteral("before?generation=20"),
        &decoded_size,
        {}
    );
    require(!before.isNull() && before.pixelColor(0, 0) == QColor(Qt::blue),
            "the before URL must decode the before slot");
    require(
        provider.requestImage(QStringLiteral("before?generation=10"), nullptr, {}).isNull(),
        "a generation from the other slot must be rejected"
    );
    require(provider.requestImage(QStringLiteral("raw?generation=20"), nullptr, {}).isNull(),
            "an unknown semantic slot must be rejected");
}

void interactive_rgb8_overview_skips_image_decode_and_retains_store_pixels() {
    auto store = std::make_shared<EditPreviewStore>();
    store->publish(EditPreviewSlot::Current, rgb_square(Qt::green), QSize(2, 2), 6, {}, 11);
    const auto stored_pixels = store->snapshot(EditPreviewSlot::Current, 11);
    const auto* const stored_address =
        reinterpret_cast<const uchar*>(stored_pixels.bytes.constData());
    EditPreviewProvider provider(store);

    QSize size;
    const QImage current =
        provider.requestImage(QStringLiteral("current?generation=11"), &size, QSize(1, 1));
    require(!current.isNull() && size == QSize(2, 2) &&
                current.pixelColor(0, 0) == QColor(Qt::green),
            "interactive RGB8 overview must bypass encoded-image decoding");
    require(current.colorSpace() == QColorSpace(QColorSpace::SRgb),
            "interactive RGB8 overview must carry display-sRGB color identity");
    require(current.constBits() == stored_address,
            "interactive RGB8 overview must retain the immutable store bytes "
            "without a copy");
}

void owned_frame_survives_store_and_texture_factory_lifetimes() {
    auto store = std::make_shared<EditPreviewStore>();
    EditPreviewProvider provider(store);
    require(
        provider.imageType() == QQmlImageProviderBase::Texture,
        "the production provider must route QML through requestTexture"
    );

    const auto destructions = std::make_shared<std::atomic_uint32_t>(0U);
    auto frame = std::make_shared<TestOwnedPreviewFrame>(Qt::green, true, destructions);
    std::weak_ptr<const BackendEditPreviewFrame> weak_frame = frame;
    const auto* const rgb_pointer = frame->materializeRgb8().data();
    require(
        frame->materializationCount() == 1U,
        "the explicit test inspection must account for one RGB materialization"
    );
    const auto* const coverage_pointer = frame->maskCoverage()->samples.data();
    constexpr MaskCoverageGeneration generation{
        .photo = 7U,
        .recipe_revision = 11U,
        .target_layer_index = 2U,
        .selection_revision = 5U,
        .paired_preview_generation = 17U,
    };
    store->expectMaskCoverage(generation);
    store->publish(
        EditPreviewSlot::Current,
        {},
        QSize(2, 2),
        6,
        {},
        generation.paired_preview_generation,
        frame
    );
    require(
        store->publishMaskCoverage(owner_coverage_descriptor(), generation),
        "paired mask coverage must publish from the current frame owner"
    );

    auto preview_snapshot =
        store->snapshot(EditPreviewSlot::Current, generation.paired_preview_generation);
    auto coverage_snapshot = store->maskCoverageSnapshot(generation);
    require(
        preview_snapshot.frame == frame && coverage_snapshot.frame == frame
            && preview_snapshot.frame->materializeRgb8().data() == rgb_pointer
            && coverage_snapshot.frame->maskCoverage()->samples.data() == coverage_pointer,
        "RGB8 and R8 snapshots must retain the exact same owner and pointers"
    );
    preview_snapshot.frame.reset();
    coverage_snapshot.frame.reset();

    require(
        provider.requestTexture(QStringLiteral("current?generation=16"), nullptr, {}) == nullptr,
        "a stale preview generation cannot obtain a texture factory"
    );
    std::unique_ptr<QQuickTextureFactory> rgb_factory(
        provider.requestTexture(QStringLiteral("current?generation=17"), nullptr, {})
    );
    std::unique_ptr<QQuickTextureFactory> mask_factory(
        provider.requestTexture(mask_provider_request(generation), nullptr, {})
    );
    require(
        rgb_factory != nullptr && mask_factory != nullptr
            && rgb_factory->textureSize() == QSize(2, 2)
            && mask_factory->textureSize() == QSize(2, 2),
        "valid RGB8 and R8 requests must produce texture factories"
    );
    require(
        frame->materializationCount() == 2U,
        "loading-thread texture factory creation must not materialize RGB"
    );
    const QImage coverage_copy = mask_factory->image();
    require(
        frame->materializationCount() == 2U,
        "R8 mask presentation must never materialize paired RGB"
    );
    const QImage rgb_copy = rgb_factory->image();
    require(
        frame->materializationCount() == 3U,
        "factory image() is the explicit host compatibility materializer"
    );
    require(
        rgb_copy.constBits() != rgb_pointer && coverage_copy.constBits() != coverage_pointer,
        "factory image() must deep-copy borrowed storage"
    );

    store->clear(EditPreviewSlot::Current, 18U);
    frame.reset();
    require(
        !weak_frame.expired(),
        "texture factories must retain the old owner after store replacement"
    );
    rgb_factory.reset();
    require(
        !weak_frame.expired(),
        "the paired mask factory must retain the shared owner independently"
    );
    mask_factory.reset();
    require(
        weak_frame.expired() && destructions->load(std::memory_order_relaxed) == 1U,
        "the shared owner must release after its final factory"
    );
    require(
        rgb_copy.pixelColor(0, 0) == QColor(Qt::green) && coverage_copy.constScanLine(1)[1] == 255U,
        "factory image() copies must remain readable after owner destruction"
    );

    auto borrowed_frame = std::make_shared<TestOwnedPreviewFrame>(Qt::blue, false, destructions);
    std::weak_ptr<const BackendEditPreviewFrame> weak_borrowed = borrowed_frame;
    store->publish(EditPreviewSlot::Current, {}, QSize(2, 2), 6, {}, 19U, borrowed_frame);
    const QImage borrowed =
        provider.requestImage(QStringLiteral("current?generation=19"), nullptr, {});
    store->clear(EditPreviewSlot::Current, 20U);
    borrowed_frame.reset();
    require(
        !weak_borrowed.expired() && borrowed.pixelColor(0, 0) == QColor(Qt::blue),
        "a borrowed QImage cleanup owner must survive store clear"
    );
}

void texture_requests_are_safe_across_loading_threads() {
    auto store = std::make_shared<EditPreviewStore>();
    EditPreviewProvider provider(store);
    const auto destructions = std::make_shared<std::atomic_uint32_t>(0U);
    auto frame = std::make_shared<TestOwnedPreviewFrame>(Qt::red, false, destructions);
    store->publish(EditPreviewSlot::Current, {}, QSize(2, 2), 6, {}, 30U, frame);

    std::atomic_bool valid{true};
    std::vector<std::thread> workers;
    workers.reserve(8U);
    for (std::size_t index = 0U; index < 8U; ++index) {
        workers.emplace_back([&provider, &valid] {
            std::unique_ptr<QQuickTextureFactory> factory(
                provider.requestTexture(QStringLiteral("current?generation=30"), nullptr, {})
            );
            if (factory == nullptr || factory->textureSize() != QSize(2, 2)
                || factory->image().pixelColor(0, 0) != QColor(Qt::red)) {
                valid.store(false, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    require(
        valid.load(std::memory_order_relaxed),
        "parallel loading-thread requests must observe one immutable frame"
    );
}

void oversized_mask_descriptor_fails_without_signed_overflow() {
    EditPreviewStore store;
    const auto frame = std::make_shared<OversizedDescriptorFrame>();
    constexpr MaskCoverageGeneration generation{
        .photo = 8U,
        .recipe_revision = 12U,
        .target_layer_index = 2U,
        .selection_revision = 5U,
        .paired_preview_generation = 31U,
    };
    store.expectMaskCoverage(generation);
    store.publish(
        EditPreviewSlot::Current,
        {},
        frame->dimensions(),
        static_cast<qsizetype>(frame->rowStrideBytes()),
        {},
        generation.paired_preview_generation,
        frame
    );
    EditMaskCoveragePayload descriptor{
        .dimensions = frame->dimensions(),
        .row_stride_bytes = static_cast<std::uint32_t>(std::numeric_limits<int>::max()),
        .version = EDIT_MASK_COVERAGE_VERSION,
        .target_layer_index = generation.target_layer_index,
        .selection_revision = generation.selection_revision,
    };
    require(
        !store.publishMaskCoverage(std::move(descriptor), generation),
        "an allocation-free INT_MAX coverage descriptor must fail closed "
        "without signed multiplication"
    );
}

void detail_tiles_are_atomic_and_generation_guarded() {
    auto store = std::make_shared<EditPreviewStore>();
    constexpr EditDetailGeneration first{
        .photo = 4,
        .recipe_revision = 9,
        .viewport_revision = 2,
    };
    QVector<EditPreviewStore::DetailPublication> publications;
    publications.push_back({
        .ticket = QStringLiteral("0-0"),
        .bytes = rgb_square(Qt::green),
        .dimensions = QSize(2, 2),
        .row_stride_bytes = 6,
    });
    store->publishDetails(std::move(publications), first);
    const auto stored_pixels = store->detailSnapshot(QStringLiteral("0-0"), first);
    const auto* const stored_address = reinterpret_cast<const uchar*>(
        stored_pixels.bytes.constData()
    );
    EditPreviewProvider provider(store);

    const QImage current = provider.requestImage(
        QStringLiteral("detail/0-0?photo=4&recipe=9&viewport=2"),
        nullptr,
        {}
    );
    require(
        !current.isNull() && current.pixelColor(0, 0) == QColor(Qt::green),
        "a detail URL must resolve only its exact generation"
    );
    require(
        current.colorSpace() == QColorSpace(QColorSpace::SRgb),
        "raw detail pixels must carry an explicit display-sRGB contract"
    );
    require(current.constBits() == stored_address,
            "detail provider must retain the immutable store bytes without a "
            "viewport copy");
    require(
        provider
            .requestImage(
                QStringLiteral("detail/0-0?photo=4&recipe=10&viewport=2"),
                nullptr,
                {}
            )
            .isNull(),
        "a stale Recipe generation must not address detail pixels"
    );

    store->clearDetails(EditDetailGeneration{
        .photo = 4,
        .recipe_revision = 9,
        .viewport_revision = 3,
    });
    require(
        provider
            .requestImage(
                QStringLiteral("detail/0-0?photo=4&recipe=9&viewport=2"),
                nullptr,
                {}
            )
            .isNull(),
        "advancing the viewport invalidates every prior tile atomically"
    );
    require(current.pixelColor(0, 0) == QColor(Qt::green),
            "an image already handed to Qt must retain its pixels after store "
            "invalidation");

    constexpr EditDetailGeneration malformed{
        .photo = 4,
        .recipe_revision = 9,
        .viewport_revision = 4,
    };
    QVector<EditPreviewStore::DetailPublication> malformed_publications;
    malformed_publications.push_back({
        .ticket = QStringLiteral("bad-stride"),
        .bytes = rgb_square(Qt::red),
        .dimensions = QSize(2, 2),
        .row_stride_bytes = 5,
    });
    store->publishDetails(std::move(malformed_publications), malformed);
    require(
        provider
            .requestImage(
                QStringLiteral("detail/bad-stride?photo=4&recipe=9&viewport=4"),
                nullptr,
                {}
            )
            .isNull(),
        "detail provider must reject non-tight RGB8 rows"
    );
}

void stale_result_rules_are_kind_specific() {
    constexpr EditPreviewGeneration current{
        .policy = EditPreviewPolicy::Settled,
        .photo = 4,
        .current_revision = 9,
    };
    constexpr EditPreviewGeneration before{
        .policy = EditPreviewPolicy::NeutralBefore,
        .photo = 4,
        .current_revision = 0,
    };
    static_assert(accepts_edit_preview(current, 4, 9));
    static_assert(!accepts_edit_preview(current, 4, 10));
    static_assert(!accepts_edit_preview(current, 5, 9));
    static_assert(accepts_edit_preview(before, 4, 99));
    static_assert(!accepts_edit_preview(before, 5, 99));
    static_assert(can_present_edit_preview(current, 4, 9));
    static_assert(can_present_edit_preview(current, 4, 10));
    static_assert(!can_present_edit_preview(current, 5, 10));
    static_assert(!can_present_edit_preview(before, 4, 10));

    static_assert(edit_preview_kind(EditPreviewPolicy::Interactive)
                  == EditPreviewKind::Current);
    static_assert(edit_preview_kind(EditPreviewPolicy::Settled)
                  == EditPreviewKind::Current);
    static_assert(edit_preview_kind(EditPreviewPolicy::NeutralBefore)
                  == EditPreviewKind::NeutralBefore);
    static_assert(!edit_preview_requires_analysis(EditPreviewPolicy::Interactive));
    static_assert(edit_preview_requires_analysis(EditPreviewPolicy::Settled));
    static_assert(edit_preview_requires_analysis(EditPreviewPolicy::NeutralBefore));
    static_assert(!edit_preview_admits_durable_cache(EditPreviewPolicy::Interactive));
    static_assert(edit_preview_admits_durable_cache(EditPreviewPolicy::Settled));
    static_assert(!edit_preview_admits_durable_cache(EditPreviewPolicy::NeutralBefore));
    static_assert(
        !edit_preview_requires_display_diagnostics(EditPreviewPolicy::Interactive)
    );
    static_assert(edit_preview_terminal_admits_publication(
        EditPreviewTerminal::Completed
    ));
    static_assert(!edit_preview_terminal_admits_publication(
        EditPreviewTerminal::Cancelled
    ));
    static_assert(!edit_preview_terminal_admits_publication(
        EditPreviewTerminal::Failed
    ));

    constexpr EditDetailGeneration detail{
        .photo = 4,
        .recipe_revision = 9,
        .viewport_revision = 3,
    };
    static_assert(accepts_edit_detail(detail, 4, 9, 3));
    static_assert(!accepts_edit_detail(detail, 5, 9, 3));
    static_assert(!accepts_edit_detail(detail, 4, 10, 3));
    static_assert(!accepts_edit_detail(detail, 4, 9, 4));
}

void before_waits_for_the_latest_current_preview() {
    constexpr NeutralBeforeStartState ready{
        .requested = true,
        .active = true,
        .settled_current_revision = 12,
        .current_revision = 12,
    };
    static_assert(can_start_neutral_before(ready));

    auto unsettled = ready;
    unsettled.current_revision = 13;
    require(
        !can_start_neutral_before(unsettled),
        "before must wait for the latest current revision to settle"
    );
    auto rendering = ready;
    rendering.current_rendering = true;
    require(
        !can_start_neutral_before(rendering),
        "before must never render concurrently with current"
    );
    auto scheduled = ready;
    scheduled.current_scheduled = true;
    require(
        !can_start_neutral_before(scheduled),
        "before must not jump ahead of a debounced current render"
    );
}

void first_interactive_frame_is_the_only_sample_protected_from_replacement() {
    constexpr EditPreviewCancellationState first{
        .current_rendering = true,
        .in_flight_policy = EditPreviewPolicy::Interactive,
        .gesture_active = true,
    };
    static_assert(!should_cancel_edit_preview(first));

    auto after_first = first;
    after_first.first_interactive_frame_presented = true;
    require(
        should_cancel_edit_preview(after_first),
        "interactive frames after the first presentation must be replaceable"
    );
    auto gesture_end = first;
    gesture_end.force = true;
    require(should_cancel_edit_preview(gesture_end),
            "gesture end must replace even a protected first frame with settled "
            "output");
    auto settled = first;
    settled.in_flight_policy = EditPreviewPolicy::Settled;
    require(
        should_cancel_edit_preview(settled),
        "a stale settled frame must be replaceable"
    );
    auto before = first;
    before.current_rendering = false;
    before.in_flight_policy = EditPreviewPolicy::NeutralBefore;
    require(
        should_cancel_edit_preview(before),
        "Neutral Before must yield when a current edit is queued"
    );
}

[[nodiscard]] double median_provider_request_ms(EditPreviewProvider& provider, const QString& url) {
    constexpr std::size_t sample_count = 11U;
    std::vector<double> samples;
    samples.reserve(sample_count);
    std::uint64_t checksum = 0U;
    for (std::size_t sample = 0U; sample < sample_count; ++sample) {
        QElapsedTimer timer;
        timer.start();
        const QImage image = provider.requestImage(url, nullptr, {});
        samples.push_back(static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0);
        require(!image.isNull(), "benchmark preview must load");
        const auto byte_count = static_cast<std::size_t>(image.sizeInBytes());
        checksum += image.constBits()[sample % byte_count];
    }
    std::ranges::sort(samples);
    require(checksum > 0U, "benchmark must consume the requested images");
    return samples[samples.size() / 2U];
}

void benchmark_transport_when_requested() {
    if (std::getenv("SHADOW_TEST_EDIT_PREVIEW_TRANSPORT_BENCHMARK") == nullptr) {
        return;
    }
    constexpr int width = 1'536;
    constexpr int height = 1'024;
    constexpr int stride = width * 3;
    QByteArray rgb(stride * height, Qt::Uninitialized);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const qsizetype offset = static_cast<qsizetype>(y) * stride + x * 3;
            rgb[offset] = static_cast<char>((x + y) % 256);
            rgb[offset + 1] = static_cast<char>((x * 3 + y) % 256);
            rgb[offset + 2] = static_cast<char>((x + y * 5) % 256);
        }
    }
    const QImage rgb_view(reinterpret_cast<const uchar*>(rgb.constData()), width, height, stride,
                          QImage::Format_RGB888);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    require(buffer.open(QIODevice::WriteOnly), "benchmark JPEG buffer must open");
    require(rgb_view.save(&buffer, "JPEG", 90), "benchmark preview must encode as JPEG");

    auto store = std::make_shared<EditPreviewStore>();
    EditPreviewProvider provider(store);
    store->publish(EditPreviewSlot::Current, rgb, QSize(width, height), stride, {}, 40);
    const double rgb_ms =
        median_provider_request_ms(provider, QStringLiteral("current?generation=40"));
    store->publish(EditPreviewSlot::Current, jpeg, QSize(width, height), 0, {}, 41);
    const double jpeg_ms =
        median_provider_request_ms(provider, QStringLiteral("current?generation=41"));
    std::cout << "BENCH preview-transport " << width << 'x' << height << " rgb8-wrap-ms=" << rgb_ms
              << " jpeg-decode-ms=" << jpeg_ms << " provider-speedup=" << jpeg_ms / rgb_ms << "x\n";
}

} // namespace

int main() {
    slots_have_independent_generations();
    provider_routes_only_named_slots();
    interactive_rgb8_overview_skips_image_decode_and_retains_store_pixels();
    owned_frame_survives_store_and_texture_factory_lifetimes();
    texture_requests_are_safe_across_loading_threads();
    oversized_mask_descriptor_fails_without_signed_overflow();
    detail_tiles_are_atomic_and_generation_guarded();
    stale_result_rules_are_kind_specific();
    before_waits_for_the_latest_current_preview();
    first_interactive_frame_is_the_only_sample_protected_from_replacement();
    benchmark_transport_when_requested();
    return EXIT_SUCCESS;
}

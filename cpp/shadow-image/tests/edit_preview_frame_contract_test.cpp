#include "contract_test_assertions.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/edit_preview_frame.hpp>

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

[[nodiscard]] image::EncodedProxy rgb8_2x2() {
    return image::EncodedProxy{
        .dimensions = {2U, 2U},
        .format = image::PreviewFormat::bitmap,
        .bits_per_channel = 8U,
        .channels = 3U,
        .bytes = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 11U},
    };
}

[[nodiscard]] image::EditPreviewMaskCoverage coverage_2x2() {
    return image::EditPreviewMaskCoverage{
        .version = std::string(image::edit_preview_mask_coverage_version),
        .layer_index = 2U,
        .dimensions = {2U, 2U},
        .row_stride_bytes = 2U,
        .samples = {0U, 64U, 128U, 255U},
    };
}

void moved_frame_keeps_renderer_allocations_and_stable_addresses() {
    auto preview = rgb8_2x2();
    auto coverage = coverage_2x2();
    const auto* const rgb_allocation = preview.bytes.data();
    const auto* const coverage_allocation = coverage.samples.data();

    image::InteractiveEditPreviewFrame frame(
        std::move(preview),
        std::move(coverage)
    );
    expect(
        frame.pixels().data() == rgb_allocation,
        "interactive frame takes ownership of the renderer RGB8 allocation"
    );
    expect(
        frame.mask_coverage() != nullptr
            && frame.mask_coverage()->samples.data() == coverage_allocation,
        "interactive frame takes paired coverage into the same owner"
    );

    image::InteractiveEditPreviewFrame moved(std::move(frame));
    expect(
        moved.dimensions() == image::Dimensions{2U, 2U}
            && moved.row_stride_bytes() == 6U
            && moved.pixels().data() == rgb_allocation
            && moved.pixels().size() == 12U,
        "moving the native owner preserves its descriptor and RGB8 address"
    );
    expect(
        moved.mask_coverage() != nullptr
            && moved.mask_coverage()->samples.data() == coverage_allocation
            && moved.mask_coverage()->samples.size() == 4U,
        "moving the native owner preserves the paired R8 address"
    );
}

template <typename Mutate>
void expect_invalid_frame(Mutate mutate, const char* message) {
    auto preview = rgb8_2x2();
    auto coverage = coverage_2x2();
    mutate(preview, coverage);
    try {
        static_cast<void>(image::InteractiveEditPreviewFrame(
            std::move(preview),
            std::move(coverage)
        ));
        expect(false, message);
    } catch (const image::DecodeError& error) {
        expect(error.code() == image::DecodeErrorCode::internal, message);
    }
}

void invalid_or_unpaired_descriptors_fail_closed() {
    expect_invalid_frame(
        [](image::EncodedProxy& preview, image::EditPreviewMaskCoverage&) {
            preview.bits_per_channel = 16U;
        },
        "interactive frame rejects a non-RGB8 descriptor"
    );
    expect_invalid_frame(
        [](image::EncodedProxy& preview, image::EditPreviewMaskCoverage&) {
            preview.bytes.pop_back();
        },
        "interactive frame rejects a truncated RGB8 allocation"
    );
    expect_invalid_frame(
        [](image::EncodedProxy&, image::EditPreviewMaskCoverage& coverage) {
            coverage.dimensions = {1U, 4U};
            coverage.row_stride_bytes = 1U;
        },
        "interactive frame rejects coverage with different paired dimensions"
    );
    expect_invalid_frame(
        [](image::EncodedProxy&, image::EditPreviewMaskCoverage& coverage) {
            coverage.version = "future-mask-coverage";
        },
        "interactive frame rejects unsupported coverage semantics"
    );
}

} // namespace

int main() {
    moved_frame_keeps_renderer_allocations_and_stable_addresses();
    invalid_or_unpaired_descriptors_fail_closed();

    if (failures != 0) {
        std::cerr << failures << " edit-preview frame contract assertion(s) failed\n";
        return 1;
    }
    return 0;
}

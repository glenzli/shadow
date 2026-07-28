#include "contract_test_assertions.hpp"

#include <shadow/image/decoder_metadata.hpp>

#include <array>
#include <cstdlib>

namespace image = shadow::image;

namespace {

using shadow::image::test_support::expect;

void largest_decodable_preview_wins() {
    const std::array previews{
        image::PreviewDescriptor{
            .id = 2,
            .format = image::PreviewFormat::bitmap,
            .dimensions = {160, 120},
            .bits_per_channel = 8,
            .channels = 3,
            .encoded_bytes = 57'600,
            .decodable = true,
        },
        image::PreviewDescriptor{
            .id = 7,
            .format = image::PreviewFormat::jpeg,
            .dimensions = {3'872, 2'592},
            .bits_per_channel = 8,
            .channels = 3,
            .encoded_bytes = 1'285'213,
            .decodable = true,
        },
        image::PreviewDescriptor{
            .id = 11,
            .format = image::PreviewFormat::jpeg,
            .dimensions = {3'872, 2'592},
            .bits_per_channel = 8,
            .channels = 3,
            .encoded_bytes = 1'500'000,
            .decodable = true,
        },
        image::PreviewDescriptor{
            .id = 9,
            .format = image::PreviewFormat::unknown,
            .dimensions = {8'000, 6'000},
            .bits_per_channel = 8,
            .channels = 3,
            .encoded_bytes = 2'000'000,
            .decodable = false,
        },
    };

    expect(
        image::select_best_preview(previews)
            == std::optional<std::size_t>{11U},
        "selection ignores undecodable entries, compares pixels then bytes, "
        "and returns the provider id"
    );
}

void no_decodable_preview_is_a_valid_state() {
    const std::array previews{
        image::PreviewDescriptor{
            .id = 4,
            .format = image::PreviewFormat::unknown,
            .dimensions = {},
            .decodable = false,
        },
    };
    expect(
        !image::select_best_preview(previews).has_value(),
        "files without an embedded preview remain a valid state"
    );
}

void preview_format_names_are_stable_and_exhaustive() {
    expect(
        image::to_string(image::PreviewFormat::unknown) == "unknown"
            && image::to_string(image::PreviewFormat::jpeg) == "jpeg"
            && image::to_string(image::PreviewFormat::bitmap) == "bitmap"
            && image::to_string(image::PreviewFormat::jpeg_xl) == "jpeg-xl"
            && image::to_string(image::PreviewFormat::h265) == "h265",
        "every PreviewFormat has a stable diagnostic name"
    );
    expect(
        image::to_string(static_cast<image::PreviewFormat>(255U))
            == "unknown",
        "out-of-range PreviewFormat diagnostics fail closed"
    );
}

} // namespace

int main() {
    largest_decodable_preview_wins();
    no_decodable_preview_is_a_valid_state();
    preview_format_names_are_stable_and_exhaustive();
    return shadow::image::test_support::failures == 0
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}

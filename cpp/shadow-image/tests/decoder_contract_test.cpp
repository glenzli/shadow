#include <shadow/image/decoder.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void pending_corrections_are_explicit() {
    image::PendingCorrections empty;
    expect(!empty.has_pending(), "empty correction state must not be pending");

    image::PendingCorrections stage_three{{0U, 0U, 76U}};
    expect(stage_three.has_pending(), "a DNG opcode list must be reported as pending");
}

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
            .id = 9,
            .format = image::PreviewFormat::unknown,
            .dimensions = {8'000, 6'000},
            .bits_per_channel = 8,
            .channels = 3,
            .encoded_bytes = 2'000'000,
            .decodable = false,
        },
    };

    const auto selected = image::select_best_preview(previews);
    expect(selected.has_value(), "a decodable preview should be selected");
    expect(selected == 7U, "selection returns the provider preview id, not the vector index");
}

void no_decodable_preview_is_a_valid_state() {
    const std::array previews{
        image::PreviewDescriptor{
            .id = 0,
            .format = image::PreviewFormat::unknown,
            .dimensions = {},
            .bits_per_channel = 0,
            .channels = 0,
            .encoded_bytes = 0,
            .decodable = false,
        },
    };
    expect(
        !image::select_best_preview(previews).has_value(),
        "files without an embedded preview must remain importable"
    );
}

} // namespace

int main() {
    pending_corrections_are_explicit();
    largest_decodable_preview_wins();
    no_decodable_preview_is_a_valid_state();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

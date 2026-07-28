#include "contract_test_assertions.hpp"

#include <shadow/image/decoder_types.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace image = shadow::image;

int main() {
    using shadow::image::test_support::expect;
    using shadow::image::test_support::failures;

    const image::Dimensions large{70'000U, 80'000U};
    expect(
        large.pixel_count() == 5'600'000'000ULL,
        "dimension products widen before multiplication"
    );

    image::PendingCorrections empty;
    expect(
        !empty.has_pending(),
        "an empty correction state is explicitly not pending"
    );
    for (std::size_t index = 0U; index < 3U; ++index) {
        image::PendingCorrections pending;
        pending.dng_opcode_list_bytes[index] =
            static_cast<std::uint32_t>(index + 1U);
        expect(
            pending.has_pending(),
            "any declared DNG opcode list makes corrections pending"
        );
    }

    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

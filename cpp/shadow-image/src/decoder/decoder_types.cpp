#include <shadow/image/decoder_types.hpp>

#include <algorithm>
#include <ranges>

namespace shadow::image {

std::uint64_t Dimensions::pixel_count() const noexcept {
    return static_cast<std::uint64_t>(width) * height;
}

bool PendingCorrections::has_pending() const noexcept {
    return std::ranges::any_of(
        dng_opcode_list_bytes,
        [](const std::uint32_t size) { return size != 0U; }
    );
}

} // namespace shadow::image

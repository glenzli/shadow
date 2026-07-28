#include <shadow/image/decoder_metadata.hpp>

namespace shadow::image {

std::optional<std::size_t> select_best_preview(
    const std::span<const PreviewDescriptor> previews
) noexcept {
    const PreviewDescriptor* best = nullptr;
    for (const auto& candidate : previews) {
        if (!candidate.decodable) {
            continue;
        }
        if (
            best == nullptr
            || candidate.dimensions.pixel_count()
                > best->dimensions.pixel_count()
            || (
                candidate.dimensions.pixel_count()
                    == best->dimensions.pixel_count()
                && candidate.encoded_bytes > best->encoded_bytes
            )
        ) {
            best = &candidate;
        }
    }
    return best == nullptr
        ? std::nullopt
        : std::optional<std::size_t>{best->id};
}

std::string_view to_string(const PreviewFormat format) noexcept {
    switch (format) {
    case PreviewFormat::jpeg:
        return "jpeg";
    case PreviewFormat::bitmap:
        return "bitmap";
    case PreviewFormat::jpeg_xl:
        return "jpeg-xl";
    case PreviewFormat::h265:
        return "h265";
    case PreviewFormat::unknown:
        return "unknown";
    }
    return "unknown";
}

} // namespace shadow::image

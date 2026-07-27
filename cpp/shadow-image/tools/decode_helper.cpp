#include <shadow/image/decoder.hpp>
#include <shadow/image/full_edit_detail.hpp>
#include <shadow/image/proxy_rendering.hpp>
#include <shadow/image/raw_pipeline.hpp>

#include <array>
#include <cmath>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
namespace image = shadow::image;

constexpr std::size_t max_identity_text_bytes = 16U * 1024U;
constexpr std::size_t max_metadata_text_bytes = 4U * 1024U;
// A complete inspection snapshot crosses the helper boundary much more often
// than an explicit metadata request. Keep its text fields deliberately tighter
// so the fixed-size parent stdout drain remains a real bound even for hostile
// EXIF/XMP strings. This protocol never transports preview pixels or paths.
constexpr std::size_t max_decoder_snapshot_identity_text_bytes = 4U * 1024U;
constexpr std::size_t max_decoder_snapshot_metadata_text_bytes = 1U * 1024U;
constexpr std::size_t max_decoder_snapshot_previews = 64U;
constexpr std::uint32_t max_detail_tile_side = 1024U;

[[nodiscard]] bool is_uuid_style_nonce(const std::string_view nonce) {
    if (nonce.size() == 32U) {
        for (const char character : nonce) {
            if (!((character >= '0' && character <= '9')
                  || (character >= 'a' && character <= 'f'))) {
                return false;
            }
        }
        return true;
    }
    if (nonce.size() != 36U) {
        return false;
    }
    for (std::size_t index = 0U; index < nonce.size(); ++index) {
        const char character = nonce[index];
        const bool is_hyphen = index == 8U || index == 13U || index == 18U || index == 23U;
        const bool is_lower_hex = (character >= '0' && character <= '9')
            || (character >= 'a' && character <= 'f');
        if (is_hyphen ? character != '-' : !is_lower_hex) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint32_t parse_edge(const std::string_view value) {
    std::uint32_t parsed = 0U;
    const auto [cursor, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed
    );
    if (error != std::errc{} || cursor != value.data() + value.size() || parsed == 0U) {
        throw std::runtime_error("max edge must be a non-zero unsigned integer");
    }
    return parsed;
}

[[nodiscard]] std::uint32_t parse_tile_coordinate(
    const std::string_view value,
    const std::string_view label,
    const bool require_non_zero
) {
    std::uint32_t parsed = 0U;
    const auto [cursor, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed
    );
    if (
        error != std::errc{} || cursor != value.data() + value.size()
        || (require_non_zero && parsed == 0U)
    ) {
        throw std::runtime_error(std::string(label) + " must be a valid unsigned integer");
    }
    return parsed;
}

[[nodiscard]] std::uint8_t parse_quality(const std::string_view value) {
    std::uint32_t parsed = 0U;
    const auto [cursor, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed
    );
    if (
        error != std::errc{} || cursor != value.data() + value.size()
        || parsed == 0U || parsed > 100U
    ) {
        throw std::runtime_error("JPEG quality must be in 1..=100");
    }
    return static_cast<std::uint8_t>(parsed);
}

void write_proxy(const fs::path& path, const image::EncodedProxy& proxy) {
    std::error_code error;
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, error);
        if (error) {
            throw std::runtime_error("cannot create proxy output directory: " + error.message());
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create proxy output");
    }
    output.write(
        reinterpret_cast<const char*>(proxy.bytes.data()),
        static_cast<std::streamsize>(proxy.bytes.size())
    );
    if (!output) {
        throw std::runtime_error("cannot write proxy output");
    }
}

int render_proxy(
    const fs::path& input,
    const fs::path& output,
    const std::uint32_t max_edge,
    const std::uint8_t jpeg_quality
) {
    const auto provider = image::make_photo_decoder_provider();
    const auto session = provider->open(input);
    const auto proxy = image::render_reference_proxy_jpeg(
        *session,
        image::ProxyRequest{max_edge, jpeg_quality}
    );
    write_proxy(output, proxy);
    // Stable, deliberately tiny stdout protocol consumed by the desktop
    // process.  The JPEG itself travels through a private cache-root file, so
    // large proxy bytes never need a pipe buffer in the crash-isolation path.
    std::cout << "shadow-proxy-v1 " << proxy.dimensions.width << ' ' << proxy.dimensions.height
              << " 8 3\n";
    return 0;
}

void write_detail_tile_atomically(
    const fs::path& output_path,
    const std::span<const std::uint8_t> bytes,
    const std::string_view nonce
) {
    if (!is_uuid_style_nonce(nonce)) {
        throw std::runtime_error("detail tile nonce must be a non-empty UUID-style token");
    }
    if (bytes.empty()) {
        throw std::runtime_error("detail tile output must not be empty");
    }
    std::error_code error;
    const fs::path parent = output_path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, error);
        if (error) {
            throw std::runtime_error(
                "cannot create detail tile output directory: " + error.message()
            );
        }
    }
    fs::path temporary = output_path;
    temporary += ".shadow-detail-partial-";
    temporary += nonce;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create detail tile temporary output");
        }
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        );
        if (!output) {
            output.close();
            fs::remove(temporary, error);
            throw std::runtime_error("cannot write detail tile temporary output");
        }
    }
    fs::rename(temporary, output_path, error);
    if (error) {
        const std::string diagnostic = error.message();
        fs::remove(temporary, error);
        throw std::runtime_error("cannot publish detail tile output: " + diagnostic);
    }
}

// This deliberately does not emit user metadata. It establishes only that the
// provider opened the source and exercised its metadata/capability/preview
// descriptor accessors inside the child process. The desktop host must not
// mistake this for a RawFrame, preview-byte, detail, or export safety receipt.
int probe_open_metadata(const fs::path& input) {
    const auto provider = image::make_photo_decoder_provider();
    const auto session = provider->open(input);
    (void)provider->info();
    (void)session->metadata();
    (void)session->capabilities();
    (void)session->previews();
    std::cout << "shadow-probe-v1 open-metadata\n";
    return 0;
}

// The v2 receipt is line-oriented and deliberately contains only hex-encoded
// implementation identities. It exposes neither user metadata nor local file
// paths. The nonce is supplied by the desktop process and makes a stale child
// stdout file impossible to mistake for the current request.
[[nodiscard]] std::string hex_encode_bounded(
    const std::string_view value,
    const std::size_t maximum_bytes,
    const std::string_view label
) {
    if (value.size() > maximum_bytes) {
        throw std::runtime_error(
            "isolated helper " + std::string(label) + " exceeds the protocol size bound"
        );
    }
    // A one-character sentinel keeps an absent EXIF string as a real field in
    // the whitespace-delimited protocol. Hex output can never contain '-'.
    if (value.empty()) {
        return "-";
    }
    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back(digits[byte >> 4U]);
        encoded.push_back(digits[byte & 0x0fU]);
    }
    return encoded;
}

[[nodiscard]] std::string fixed_hex_u64(const std::uint64_t value) {
    constexpr std::array<char, 16> digits{
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::array<char, 16> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const auto shift = static_cast<unsigned int>((encoded.size() - index - 1U) * 4U);
        encoded[index] = digits[(value >> shift) & 0x0fU];
    }
    return std::string(encoded.data(), encoded.size());
}

[[nodiscard]] std::string fixed_hex_f64(const double value, const std::string_view label) {
    if (!std::isfinite(value)) {
        throw std::runtime_error(
            "isolated helper metadata " + std::string(label) + " is not finite"
        );
    }
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t bits = 0U;
    std::memcpy(&bits, &value, sizeof(bits));
    return fixed_hex_u64(bits);
}

// Keep the exact Rust RawMetadataSnapshot field order in one place. Every
// numeric field is an IEEE/integer bit string, so child and parent locale do
// not affect the protocol. `focus_distance_meters` deliberately stays out:
// it is not part of the current public Rust snapshot contract.
void append_metadata_snapshot_fields(
    std::ostream& output,
    const image::AssetMetadata& metadata,
    const std::size_t maximum_text_bytes
) {
    output
        << ' ' << hex_encode_bounded(metadata.make, maximum_text_bytes, "make")
        << ' ' << hex_encode_bounded(metadata.model, maximum_text_bytes, "model")
        << ' ' << hex_encode_bounded(
               metadata.normalized_make, maximum_text_bytes, "normalized make"
           )
        << ' ' << hex_encode_bounded(
               metadata.normalized_model, maximum_text_bytes, "normalized model"
           )
        << ' ' << hex_encode_bounded(metadata.dng_version, maximum_text_bytes, "DNG version")
        << ' ' << fixed_hex_u64(metadata.raw_count)
        << ' ' << fixed_hex_u64(metadata.raw_dimensions.width)
        << ' ' << fixed_hex_u64(metadata.raw_dimensions.height)
        << ' ' << fixed_hex_u64(metadata.image_dimensions.width)
        << ' ' << fixed_hex_u64(metadata.image_dimensions.height)
        << ' ' << fixed_hex_u64(metadata.margins.left)
        << ' ' << fixed_hex_u64(metadata.margins.top)
        << ' ' << fixed_hex_u64(metadata.margins.right)
        << ' ' << fixed_hex_u64(metadata.margins.bottom)
        << ' ' << fixed_hex_u64(static_cast<std::uint32_t>(metadata.orientation))
        << ' ' << hex_encode_bounded(metadata.cfa_pattern, maximum_text_bytes, "CFA pattern")
        << ' ' << fixed_hex_u64(metadata.sensor_colors)
        << ' ' << fixed_hex_u64(metadata.sensor_bits)
        << ' ' << fixed_hex_u64(metadata.black_level)
        << ' ' << fixed_hex_u64(metadata.white_level)
        << ' ' << fixed_hex_f64(metadata.as_shot_neutral[0], "as-shot neutral red")
        << ' ' << fixed_hex_f64(metadata.as_shot_neutral[1], "as-shot neutral green 1")
        << ' ' << fixed_hex_f64(metadata.as_shot_neutral[2], "as-shot neutral blue")
        << ' ' << fixed_hex_f64(metadata.as_shot_neutral[3], "as-shot neutral green 2")
        << ' ' << fixed_hex_f64(metadata.baseline_exposure, "baseline exposure")
        << ' ' << fixed_hex_f64(metadata.iso_speed, "ISO speed")
        << ' ' << fixed_hex_f64(metadata.exposure_time_seconds, "exposure time")
        << ' ' << fixed_hex_f64(metadata.aperture_f_number, "aperture")
        << ' ' << fixed_hex_f64(metadata.focal_length_mm, "focal length")
        << ' ' << fixed_hex_u64(static_cast<std::uint64_t>(metadata.captured_at_unix_seconds))
        << ' ' << hex_encode_bounded(metadata.lens_make, maximum_text_bytes, "lens make")
        << ' ' << hex_encode_bounded(metadata.lens_model, maximum_text_bytes, "lens model")
        << ' ' << fixed_hex_f64(metadata.focal_length_35mm, "35 mm focal length");
}

void append_decoder_snapshot_capability_fields(
    std::ostream& output,
    const image::DecodeCapabilities& capabilities,
    const image::RawDevelopmentCapabilities& raw_development
) {
    const auto as_u64 = [](const bool value) {
        return value ? std::uint64_t{1U} : std::uint64_t{0U};
    };
    output
        << ' ' << fixed_hex_u64(as_u64(capabilities.metadata))
        << ' ' << fixed_hex_u64(as_u64(capabilities.embedded_previews))
        << ' ' << fixed_hex_u64(as_u64(capabilities.raw_frame))
        << ' ' << fixed_hex_u64(as_u64(capabilities.reference_rgb))
        << ' ' << fixed_hex_u64(capabilities.pending_corrections.dng_opcode_list_bytes[0])
        << ' ' << fixed_hex_u64(capabilities.pending_corrections.dng_opcode_list_bytes[1])
        << ' ' << fixed_hex_u64(capabilities.pending_corrections.dng_opcode_list_bytes[2])
        << ' ' << fixed_hex_u64(raw_development.schema_version)
        << ' ' << fixed_hex_u64(as_u64(raw_development.available))
        << ' ' << fixed_hex_u64(as_u64(raw_development.raw_frame))
        << ' ' << fixed_hex_u64(as_u64(raw_development.dng_opcode_execution_receipt))
        << ' ' << fixed_hex_u64(raw_development.supported_intents)
        << ' ' << fixed_hex_u64(raw_development.supported_qualities)
        << ' ' << fixed_hex_u64(raw_development.supported_dng_opcode_policies)
        << ' ' << fixed_hex_u64(raw_development.supported_noise_reduction_intents)
        << ' ' << fixed_hex_u64(raw_development.supported_highlight_recovery_intents);
}

// This is a real bounded preview-development probe, not a metadata probe. It
// exercises provider selection plus Shadow's effective RAW pipeline inside the
// child and returns the exact resulting route identity. It intentionally does
// not transfer pixels or certify full-detail/export: those require a later
// high-bit-depth/tiled worker protocol.
int probe_preview_development(
    const fs::path& input,
    const std::uint32_t max_edge,
    const std::string_view nonce
) {
    if (nonce.empty()) {
        throw std::runtime_error("preview-development probe nonce must be non-empty");
    }
    const auto provider = image::make_photo_decoder_provider();
    const auto session = provider->open(input);
    const auto developed = image::develop_source_reference(
        *session,
        image::preview_raw_development_plan(),
        max_edge,
        image::raw_pipeline_policy_from_environment()
    );
    const auto& receipt = developed.pipeline_receipt;
    if (!receipt.valid()) {
        throw std::runtime_error("preview-development probe received an invalid RAW pipeline receipt");
    }
    std::cout
        << "shadow-probe-v2 preview-development " << nonce << ' '
        << static_cast<unsigned int>(receipt.path) << ' '
        << hex_encode_bounded(
               receipt.source_provider_id, max_identity_text_bytes, "source provider id"
           ) << ' '
        << hex_encode_bounded(
               receipt.source_provider_version, max_identity_text_bytes, "source provider version"
           ) << ' '
        << hex_encode_bounded(
               image::raw_pipeline_receipt_identity(receipt),
               max_identity_text_bytes,
               "pipeline receipt identity"
           ) << ' '
        << hex_encode_bounded(
               image::raw_development_plan_identity(receipt.requested_plan),
               max_identity_text_bytes,
               "requested plan identity"
           ) << ' '
        << hex_encode_bounded(
               image::raw_development_plan_identity(receipt.effective_plan),
               max_identity_text_bytes,
               "effective plan identity"
           ) << '\n';
    return 0;
}

// This protocol transfers metadata only. It does not return preview bytes, a
// RawFrame, or an edit session, so a later catalog/optics caller can use the
// child-established snapshot without reopening a possibly unsafe RAW in the
// desktop process. Numeric values are fixed-width IEEE/integer bit strings to
// avoid host locale drift in a cross-process protocol.
int snapshot_metadata(const fs::path& input, const std::string_view nonce) {
    if (nonce.empty()) {
        throw std::runtime_error("metadata snapshot nonce must be non-empty");
    }
    const auto provider = image::make_photo_decoder_provider();
    const auto session = provider->open(input);
    const auto& provider_info = provider->info();
    const auto& metadata = session->metadata();
    std::cout
        << "shadow-metadata-v2 metadata-snapshot " << nonce << ' '
        << hex_encode_bounded(provider_info.id, max_identity_text_bytes, "router provider id") << ' '
        << hex_encode_bounded(
               provider_info.version, max_identity_text_bytes, "router provider version"
           );
    append_metadata_snapshot_fields(std::cout, metadata, max_metadata_text_bytes);
    std::cout << '\n';
    return 0;
}

// A complete descriptor snapshot for the catalog worker. This is the first
// helper protocol that replaces the host's direct `inspect_photo` call: it
// opens the source, copies metadata/capabilities/preview *descriptors*, then
// exits. Preview pixels, RawFrame samples and developed RGB deliberately stay
// inside the child; the host will request the existing bounded proxy stage for
// a visual instead.
int snapshot_decoder(const fs::path& input, const std::string_view nonce) {
    if (nonce.empty()) {
        throw std::runtime_error("decoder snapshot nonce must be non-empty");
    }
    const auto provider = image::make_photo_decoder_provider();
    const auto session = provider->open(input);
    const auto& provider_info = provider->info();
    const auto& metadata = session->metadata();
    const auto& capabilities = session->capabilities();
    const auto previews = session->previews();
    if (previews.size() > max_decoder_snapshot_previews) {
        throw std::runtime_error("decoder snapshot has too many preview descriptors");
    }

    // Mirror the FFI `DecodeHandle::capabilities()` contract. When a provider
    // exposes RawFrame, Shadow's host-owned RAW developer—not the provider's
    // processed-RGB compatibility path—owns capability negotiation.
    const auto raw_development = capabilities.raw_frame
        ? image::shadow_raw_frame_development_capabilities()
        : session->raw_development_capabilities();

    std::cout
        << "shadow-inspect-v3 decoder-snapshot " << nonce << ' '
        << hex_encode_bounded(
               provider_info.id,
               max_decoder_snapshot_identity_text_bytes,
               "decoder snapshot router provider id"
           ) << ' '
        << hex_encode_bounded(
               provider_info.version,
               max_decoder_snapshot_identity_text_bytes,
               "decoder snapshot router provider version"
           ) << ' '
        << fixed_hex_u64(provider_info.dng_sdk ? 1U : 0U) << ' '
        << fixed_hex_u64(provider_info.rawspeed ? 1U : 0U) << ' '
        << fixed_hex_u64(provider_info.jpeg ? 1U : 0U);
    append_metadata_snapshot_fields(
        std::cout, metadata, max_decoder_snapshot_metadata_text_bytes
    );
    append_decoder_snapshot_capability_fields(std::cout, capabilities, raw_development);
    std::cout << ' ' << fixed_hex_u64(previews.size());
    for (const auto& preview : previews) {
        std::cout
            << ' ' << fixed_hex_u64(preview.id)
            << ' ' << fixed_hex_u64(static_cast<std::uint8_t>(preview.format))
            << ' ' << fixed_hex_u64(preview.dimensions.width)
            << ' ' << fixed_hex_u64(preview.dimensions.height)
            << ' ' << fixed_hex_u64(preview.bits_per_channel)
            << ' ' << fixed_hex_u64(preview.channels)
            << ' ' << fixed_hex_u64(preview.encoded_bytes)
            << ' ' << fixed_hex_u64(preview.decodable ? 1U : 0U);
    }
    std::cout << '\n';
    return 0;
}

// The first full-detail isolation primitive intentionally renders a neutral
// tile only. It proves that source open, RawFrame/compatibility development,
// the full-resolution session and display conversion can all remain inside the
// child. Recipe/layer serialization is a separate, versioned request contract;
// this command must never silently substitute a JPEG proxy for a RAW tile.
int render_neutral_detail_tile(
    const fs::path& input,
    const fs::path& output,
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::string_view nonce
) {
    if (!is_uuid_style_nonce(nonce)) {
        throw std::runtime_error("detail tile nonce must be a UUID-style token");
    }
    if (width > max_detail_tile_side || height > max_detail_tile_side) {
        throw std::runtime_error("detail tile dimensions exceed the bounded protocol limit");
    }
    const auto provider = image::make_photo_decoder_provider();
    const auto session = provider->open(input);
    if (!session->raw_development_capabilities().available) {
        throw std::runtime_error(
            "neutral detail tile requires a source with an available RAW development contract"
        );
    }
    const auto detail = image::prepare_full_edit_detail(*session);
    const auto tile = detail.render_rgb8(
        {}, image::DetailTileRect{.x = x, .y = y, .width = width, .height = height}
    );
    const auto expected_stride = static_cast<std::uint64_t>(tile.rect.width) * 3U;
    const auto expected_bytes = expected_stride * tile.rect.height;
    if (
        tile.row_stride_bytes != expected_stride || tile.bytes.size() != expected_bytes
        || tile.full_dimensions.width == 0U || tile.full_dimensions.height == 0U
    ) {
        throw std::runtime_error("detail tile renderer returned an invalid RGB8 layout");
    }
    write_detail_tile_atomically(output, tile.bytes, nonce);
    const auto& pipeline_receipt = detail.raw_pipeline_receipt();
    std::cout
        << "shadow-detail-tile-v1 neutral-detail-tile " << nonce
        << ' ' << fixed_hex_u64(tile.rect.x)
        << ' ' << fixed_hex_u64(tile.rect.y)
        << ' ' << fixed_hex_u64(tile.rect.width)
        << ' ' << fixed_hex_u64(tile.rect.height)
        << ' ' << fixed_hex_u64(tile.full_dimensions.width)
        << ' ' << fixed_hex_u64(tile.full_dimensions.height)
        << ' ' << fixed_hex_u64(tile.row_stride_bytes)
        << ' ' << fixed_hex_u64(tile.bytes.size())
        << ' ' << fixed_hex_u64(static_cast<std::uint8_t>(pipeline_receipt.path))
        << ' ' << hex_encode_bounded(
               image::raw_pipeline_receipt_identity(pipeline_receipt),
               max_identity_text_bytes,
               "detail tile pipeline receipt identity"
           )
        << '\n';
    return 0;
}

} // namespace

int main(const int argument_count, char** arguments) {
    try {
        const std::string_view command = argument_count >= 2
            ? std::string_view(arguments[1])
            : std::string_view{};
        if (command == "proxy" && argument_count == 6) {
            return render_proxy(
                fs::path(arguments[2]),
                fs::path(arguments[3]),
                parse_edge(arguments[4]),
                parse_quality(arguments[5])
            );
        }
        if (command == "probe" && argument_count == 3) {
            return probe_open_metadata(fs::path(arguments[2]));
        }
        if (command == "preview-receipt" && argument_count == 5) {
            return probe_preview_development(
                fs::path(arguments[2]),
                parse_edge(arguments[3]),
                std::string_view(arguments[4])
            );
        }
        if (command == "metadata-snapshot" && argument_count == 4) {
            return snapshot_metadata(
                fs::path(arguments[2]),
                std::string_view(arguments[3])
            );
        }
        if (command == "decoder-snapshot" && argument_count == 4) {
            return snapshot_decoder(
                fs::path(arguments[2]),
                std::string_view(arguments[3])
            );
        }
        if (command == "neutral-detail-tile" && argument_count == 9) {
            return render_neutral_detail_tile(
                fs::path(arguments[2]),
                fs::path(arguments[3]),
                parse_tile_coordinate(arguments[4], "tile x", false),
                parse_tile_coordinate(arguments[5], "tile y", false),
                parse_tile_coordinate(arguments[6], "tile width", true),
                parse_tile_coordinate(arguments[7], "tile height", true),
                std::string_view(arguments[8])
            );
        }
        std::cerr
            << "usage: shadow-image-decode-helper proxy <input> <output> <max-edge> <jpeg-quality>\n"
            << "       shadow-image-decode-helper probe <input>\n"
            << "       shadow-image-decode-helper preview-receipt <input> <max-edge> <nonce>\n"
            << "       shadow-image-decode-helper metadata-snapshot <input> <nonce>\n"
            << "       shadow-image-decode-helper decoder-snapshot <input> <nonce>\n"
            << "       shadow-image-decode-helper neutral-detail-tile <input> <output> <x> <y> <width> <height> <nonce>\n";
        return 2;
    } catch (const image::DecodeError& error) {
        std::cerr << "shadow-image-decode-helper: " << error.what()
                  << " [provider=" << error.provider_code() << "]\n";
    } catch (const std::exception& error) {
        std::cerr << "shadow-image-decode-helper: " << error.what() << '\n';
    }
    return 1;
}

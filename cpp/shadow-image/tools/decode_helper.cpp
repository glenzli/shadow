#include <shadow/image/decoder.hpp>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
namespace image = shadow::image;

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

} // namespace

int main(const int argument_count, char** arguments) {
    if (argument_count != 6 || std::string_view(arguments[1]) != "proxy") {
        std::cerr << "usage: shadow-image-decode-helper proxy <input> <output> <max-edge> <jpeg-quality>\n";
        return 2;
    }
    try {
        return render_proxy(
            fs::path(arguments[2]),
            fs::path(arguments[3]),
            parse_edge(arguments[4]),
            parse_quality(arguments[5])
        );
    } catch (const image::DecodeError& error) {
        std::cerr << "shadow-image-decode-helper: " << error.what()
                  << " [provider=" << error.provider_code() << "]\n";
    } catch (const std::exception& error) {
        std::cerr << "shadow-image-decode-helper: " << error.what() << '\n';
    }
    return 1;
}

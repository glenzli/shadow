#include <shadow/image/decoder.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
namespace image = shadow::image;
using Clock = std::chrono::steady_clock;

class Stopwatch final {
public:
    Stopwatch() : started_at_(Clock::now()) {}

    [[nodiscard]] double elapsed_ms() const {
        const auto elapsed = Clock::now() - started_at_;
        return std::chrono::duration<double, std::milli>(elapsed).count();
    }

private:
    Clock::time_point started_at_;
};

void write_binary(
    const fs::path& path,
    const std::span<const std::uint8_t> bytes
) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot create " + path.string());
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (!output) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

void write_u16_pnm(
    const fs::path& path,
    const image::Dimensions dimensions,
    const std::uint16_t channels,
    const std::span<const std::uint16_t> samples
) {
    if (channels != 1U && channels != 3U) {
        throw std::runtime_error("PNM output requires one or three channels");
    }
    const std::uint64_t expected = dimensions.pixel_count() * channels;
    if (expected != samples.size()) {
        throw std::runtime_error("pixel buffer length does not match its dimensions");
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot create " + path.string());
    }
    output << (channels == 1U ? "P5\n" : "P6\n") << dimensions.width << ' '
           << dimensions.height << "\n65535\n";
    for (const std::uint16_t value : samples) {
        const char bytes[2] = {
            static_cast<char>((value >> 8U) & 0xffU),
            static_cast<char>(value & 0xffU),
        };
        output.write(bytes, 2);
    }
    if (!output) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

void write_bitmap_preview(const fs::path& path, const image::PreviewPayload& preview) {
    const auto& descriptor = preview.descriptor;
    if (descriptor.bits_per_channel != 8U || descriptor.channels == 0U) {
        throw std::runtime_error("probe only writes 8-bit bitmap previews");
    }
    const std::uint64_t expected = descriptor.dimensions.pixel_count() * descriptor.channels;
    if (expected != preview.bytes.size()) {
        throw std::runtime_error("bitmap preview length does not match its dimensions");
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot create " + path.string());
    }
    output << (descriptor.channels == 1U ? "P5\n" : "P6\n")
           << descriptor.dimensions.width << ' ' << descriptor.dimensions.height << "\n255\n";
    output.write(
        reinterpret_cast<const char*>(preview.bytes.data()),
        static_cast<std::streamsize>(preview.bytes.size())
    );
    if (!output) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

[[nodiscard]] fs::path preview_path(
    const fs::path& output_directory,
    const image::PreviewFormat format,
    const std::uint16_t channels
) {
    switch (format) {
    case image::PreviewFormat::jpeg:
        return output_directory / "embedded-preview.jpg";
    case image::PreviewFormat::bitmap:
        return output_directory / (channels == 1U ? "embedded-preview.pgm" : "embedded-preview.ppm");
    case image::PreviewFormat::jpeg_xl:
        return output_directory / "embedded-preview.jxl";
    case image::PreviewFormat::h265:
        return output_directory / "embedded-preview.h265";
    case image::PreviewFormat::unknown:
        return output_directory / "embedded-preview.bin";
    }
    return output_directory / "embedded-preview.bin";
}

void print_session(const image::ProviderInfo& provider, const image::DecodeSession& session) {
    const auto& metadata = session.metadata();
    const auto& capabilities = session.capabilities();
    std::cout << "provider.id=" << provider.id << '\n'
              << "provider.version=" << provider.version << '\n'
              << "provider.capability.dng_sdk=" << (provider.dng_sdk ? "yes" : "no") << '\n'
              << "provider.capability.rawspeed=" << (provider.rawspeed ? "yes" : "no") << '\n'
              << "provider.capability.jpeg=" << (provider.jpeg ? "yes" : "no") << '\n'
              << "camera.make=" << metadata.make << '\n'
              << "camera.model=" << metadata.model << '\n'
              << "camera.normalized_make=" << metadata.normalized_make << '\n'
              << "camera.normalized_model=" << metadata.normalized_model << '\n'
              << "raw.count=" << metadata.raw_count << '\n'
              << "raw.dng_version=" << metadata.dng_version << '\n'
              << "raw.dimensions=" << metadata.raw_dimensions.width << 'x'
              << metadata.raw_dimensions.height << '\n'
              << "image.dimensions=" << metadata.image_dimensions.width << 'x'
              << metadata.image_dimensions.height << '\n'
              << "image.margins=" << metadata.margins.left << ',' << metadata.margins.top << ','
              << metadata.margins.right << ',' << metadata.margins.bottom << '\n'
              << "image.flip=" << metadata.orientation << '\n'
              << "sensor.colors=" << metadata.sensor_colors << '\n'
              << "sensor.cfa=" << metadata.cfa_pattern << '\n'
              << "sensor.bits=" << metadata.sensor_bits << '\n'
              << "sensor.black=" << metadata.black_level << '\n'
              << "sensor.maximum=" << metadata.white_level << '\n'
              << "dng.as_shot_neutral=" << metadata.as_shot_neutral[0] << ','
              << metadata.as_shot_neutral[1] << ',' << metadata.as_shot_neutral[2] << ','
              << metadata.as_shot_neutral[3] << '\n'
              << "dng.baseline_exposure=" << metadata.baseline_exposure << '\n'
              << "dng.opcode_list_bytes="
              << capabilities.pending_corrections.dng_opcode_list_bytes[0] << ','
              << capabilities.pending_corrections.dng_opcode_list_bytes[1] << ','
              << capabilities.pending_corrections.dng_opcode_list_bytes[2] << '\n'
              << "decoder.capability.metadata=" << (capabilities.metadata ? "yes" : "no") << '\n'
              << "decoder.capability.embedded_previews="
              << (capabilities.embedded_previews ? "yes" : "no") << '\n'
              << "decoder.capability.raw_frame=" << (capabilities.raw_frame ? "yes" : "no") << '\n'
              << "decoder.capability.reference_rgb="
              << (capabilities.reference_rgb ? "yes" : "no") << '\n'
              << "decoder.pending_corrections="
              << (capabilities.pending_corrections.has_pending() ? "yes" : "no") << '\n'
              << "thumbnail.candidates=" << session.previews().size() << '\n';

    for (const auto& preview : session.previews()) {
        std::cout << "thumbnail.candidate." << preview.id << '=' << image::to_string(preview.format)
                  << ',' << preview.dimensions.width << 'x' << preview.dimensions.height << ','
                  << preview.bits_per_channel << "bit," << preview.channels << "color,"
                  << preview.encoded_bytes << "bytes\n";
    }
}

void extract_best_preview(image::DecodeSession& session, const fs::path& output_directory) {
    const auto selected = image::select_best_preview(session.previews());
    if (!selected) {
        std::cout << "thumbnail.status=unavailable\n";
        return;
    }

    const Stopwatch timer;
    const image::PreviewPayload preview = session.decode_preview(*selected);
    const fs::path output_path =
        preview_path(output_directory, preview.descriptor.format, preview.descriptor.channels);
    if (preview.descriptor.format == image::PreviewFormat::bitmap) {
        write_bitmap_preview(output_path, preview);
    } else {
        write_binary(output_path, preview.bytes);
    }

    std::cout << "thumbnail.status=ok\n"
              << "thumbnail.selected=" << preview.descriptor.id << '\n'
              << "thumbnail.format=" << image::to_string(preview.descriptor.format) << '\n'
              << "thumbnail.dimensions=" << preview.descriptor.dimensions.width << 'x'
              << preview.descriptor.dimensions.height << '\n'
              << "thumbnail.bits=" << preview.descriptor.bits_per_channel << '\n'
              << "thumbnail.colors=" << preview.descriptor.channels << '\n'
              << "thumbnail.bytes=" << preview.bytes.size() << '\n'
              << "thumbnail.output=" << output_path.string() << '\n'
              << "timing.thumbnail_ms=" << timer.elapsed_ms() << '\n';
}

void inspect_raw_frame(image::DecodeSession& session, const fs::path& output_directory) {
    const Stopwatch timer;
    const image::RawFrame frame = session.decode_raw_frame();
    if (!frame.valid()) {
        throw std::runtime_error("provider returned an invalid RAW frame");
    }
    const auto [minimum, maximum] = std::minmax_element(frame.samples.begin(), frame.samples.end());
    long double total = 0.0L;
    std::uint64_t checksum = 1'469'598'103'934'665'603ULL;
    for (const std::uint16_t value : frame.samples) {
        total += value;
        checksum ^= value;
        checksum *= 1'099'511'628'211ULL;
    }

    const fs::path output_path = output_directory / "raw-frame.pgm";
    write_u16_pnm(output_path, frame.descriptor.storage_dimensions, 1U, frame.samples);
    const auto sample_count = static_cast<long double>(frame.samples.size());

    std::cout << "raw_frame.status=ok\n"
              << "raw_frame.samples=" << frame.samples.size() << '\n'
              << "raw_frame.bits=" << frame.descriptor.bits_per_sample << '\n'
              << "raw_frame.cfa=" << frame.descriptor.cfa_pattern << '\n'
              << "raw_frame.bayer_2x2=" << (frame.is_bayer_2x2() ? "yes" : "no") << '\n'
              << "raw_frame.minimum=" << *minimum << '\n'
              << "raw_frame.maximum=" << *maximum << '\n'
              << "raw_frame.mean=" << static_cast<double>(total / sample_count) << '\n'
              << "raw_frame.fnv1a64=" << std::hex << std::setw(16) << std::setfill('0') << checksum
              << std::dec << std::setfill(' ') << '\n'
              << "raw_frame.output=" << output_path.string() << '\n'
              << "timing.raw_frame_ms=" << timer.elapsed_ms() << '\n';
}

void render_reference_rgb(image::DecodeSession& session, const fs::path& output_directory) {
    const Stopwatch timer;
    const auto plan = image::default_raw_development_plan();
    const image::PixelBuffer rendered = session.render_reference_rgb(plan);
    const fs::path output_path = output_directory / "reference-linear-srgb-16bit.ppm";
    write_u16_pnm(output_path, rendered.dimensions, rendered.channels, rendered.samples);

    std::cout << "reference_rgb.status=ok\n"
              << "reference_rgb.dimensions=" << rendered.dimensions.width << 'x'
              << rendered.dimensions.height << '\n'
              << "reference_rgb.bits=" << rendered.bits_per_channel << '\n'
              << "reference_rgb.colors=" << rendered.channels << '\n'
              << "reference_rgb.transfer=linear\n"
              << "reference_rgb.primaries=srgb-rec709-d65\n"
              << "reference_rgb.reference=processed-raw\n"
              << "reference_rgb.samples=" << rendered.samples.size() << '\n'
              << "reference_rgb.plan.requested="
              << rendered.raw_development_receipt.requested_plan_identity << '\n'
              << "reference_rgb.plan.effective="
              << rendered.raw_development_receipt.effective_plan_identity << '\n'
              << "reference_rgb.output=" << output_path.string() << '\n'
              << "timing.reference_rgb_ms=" << timer.elapsed_ms() << '\n';
}

void render_warm_preview_reference_rgb(
    image::DecodeSession& session,
    const fs::path& output_directory
) {
    constexpr std::uint32_t warm_preview_edge = 1'200U;
    const Stopwatch timer;
    const auto plan = image::preview_raw_development_plan();
    const image::PixelBuffer rendered = session.render_reference_rgb_for_preview(
        warm_preview_edge,
        plan
    );
    const fs::path output_path = output_directory / "preview-reference-linear-srgb-16bit.ppm";
    write_u16_pnm(output_path, rendered.dimensions, rendered.channels, rendered.samples);
    std::array<long double, 3U> channel_sum{};
    const std::size_t pixel_count = static_cast<std::size_t>(rendered.dimensions.width)
        * rendered.dimensions.height;
    for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
        for (std::size_t channel = 0U; channel < channel_sum.size(); ++channel) {
            channel_sum[channel] += rendered.samples[pixel * rendered.channels + channel];
        }
    }
    std::cout << "preview_reference.status=ok\n"
              << "preview_reference.requested_max_edge=" << warm_preview_edge << '\n'
              << "preview_reference.dimensions=" << rendered.dimensions.width << 'x'
              << rendered.dimensions.height << '\n'
              << "preview_reference.samples=" << rendered.samples.size() << '\n'
              << "preview_reference.plan.requested="
              << rendered.raw_development_receipt.requested_plan_identity << '\n'
              << "preview_reference.plan.effective="
              << rendered.raw_development_receipt.effective_plan_identity << '\n'
              << "preview_reference.linear_mean_rgb="
              << static_cast<double>(channel_sum[0] / static_cast<long double>(pixel_count)) << ','
              << static_cast<double>(channel_sum[1] / static_cast<long double>(pixel_count)) << ','
              << static_cast<double>(channel_sum[2] / static_cast<long double>(pixel_count)) << '\n'
              << "preview_reference.output=" << output_path.string() << '\n'
              << "timing.preview_reference_ms=" << timer.elapsed_ms() << '\n';
}

int run(
    const fs::path& input_path,
    const fs::path& output_directory,
    const bool preview_only
) {
    fs::create_directories(output_directory);
    // Deliberately use the same routed provider as the desktop app. With no
    // SHADOW_PRIVATE_DECODER_PLUGIN_PATH it resolves to the public LibRaw
    // implementation; with one configured it exercises a private adapter's
    // complete open/metadata/preview/RAW-frame path before falling back.
    const auto provider = image::make_photo_decoder_provider();

    const Stopwatch open_timer;
    const auto session = provider->open(input_path);
    std::cout << std::fixed << std::setprecision(3)
              << "input=" << input_path.string() << '\n'
              << "output_directory=" << output_directory.string() << '\n'
              << "timing.open_ms=" << open_timer.elapsed_ms() << '\n';
    print_session(provider->info(), *session);
    extract_best_preview(*session, output_directory);

    // Some recognized cameras can offer an embedded preview while their sensor
    // data is intentionally unavailable to the active provider (for example a
    // compressed RAW format awaiting a private adapter). That is a truthful,
    // useful preview-only result, not a probe failure and never a reason to
    // invoke LibRaw's processing path.
    if (!session->capabilities().reference_rgb) {
        std::cout << "preview_reference.status=unavailable\n"
                  << "preview_reference.reason=provider-does-not-expose-reference-rgb\n";
        if (!session->capabilities().raw_frame) {
            std::cout << "raw_frame.status=unavailable\n"
                      << "raw_frame.reason=provider-does-not-expose-raw-frame\n";
        }
        return 0;
    }
    render_warm_preview_reference_rgb(*session, output_directory);
    if (preview_only) {
        return 0;
    }
    if (!session->capabilities().raw_frame) {
        std::cout << "raw_frame.status=unavailable\n"
                  << "raw_frame.reason=provider-does-not-expose-raw-frame\n";
        render_reference_rgb(*session, output_directory);
        return 0;
    }
    inspect_raw_frame(*session, output_directory);
    render_reference_rgb(*session, output_directory);
    return 0;
}

} // namespace

int main(const int argument_count, char** arguments) {
    const bool preview_only = argument_count == 4
        && std::string_view(arguments[3]) == "--preview-only";
    if (argument_count != 3 && !preview_only) {
        std::cerr << "usage: shadow-raw-probe <input-raw> <output-directory> [--preview-only]\n";
        return 2;
    }

    try {
        return run(arguments[1], arguments[2], preview_only);
    } catch (const image::DecodeError& error) {
        std::cerr << "shadow-raw-probe: " << error.what() << " [provider="
                  << error.provider_code() << "]\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "shadow-raw-probe: " << error.what() << '\n';
        return 1;
    }
}

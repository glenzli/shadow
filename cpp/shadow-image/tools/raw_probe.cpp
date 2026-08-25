#include <shadow/image/adjustment_graph.hpp>
#include <shadow/image/adjustment_parameters.hpp>
#include <shadow/image/decoder.hpp>
#include <shadow/image/display_luma.hpp>
#include <shadow/image/raw_pipeline.hpp>
#include <shadow/image/warm_edit_preview.hpp>

#include "../src/raw/bayer_sampling.hpp"
#include "../src/raw/raw_frame_development_plan.hpp"
#include "raw_highlight_cfa_diagnostic.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

void write_binary(const fs::path& path, const std::span<const std::uint8_t> bytes) {
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

void write_u8_pgm(
    const fs::path& path,
    const image::Dimensions dimensions,
    const std::span<const std::uint8_t> samples
) {
    if (dimensions.pixel_count() != samples.size()) {
        throw std::runtime_error("PGM buffer length does not match its dimensions");
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot create " + path.string());
    }
    output << "P5\n" << dimensions.width << ' ' << dimensions.height << "\n255\n";
    output.write(
        reinterpret_cast<const char*>(samples.data()),
        static_cast<std::streamsize>(samples.size())
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
    output << (channels == 1U ? "P5\n" : "P6\n") << dimensions.width << ' ' << dimensions.height
           << "\n65535\n";
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

struct ProbeLinearSource final {
    image::Dimensions dimensions;
    std::uint16_t bits_per_channel = 16U;
    std::uint16_t channels = 3U;
    std::vector<std::uint16_t> samples;
    bool scene_linear_f32 = false;
};

[[nodiscard]] ProbeLinearSource
materialize_probe_source(const image::DevelopedSourcePixels& source) {
    if (const auto* packed = std::get_if<image::PixelBuffer>(&source)) {
        return ProbeLinearSource{
            .dimensions = packed->dimensions,
            .channels = packed->channels,
            .samples = packed->samples,
        };
    }
    const auto& scene = std::get<image::SceneLinearRgbFrame>(source);
    ProbeLinearSource result;
    result.dimensions = scene.dimensions;
    result.samples.resize(scene.samples.size());
    result.scene_linear_f32 = true;
    for (std::size_t index = 0U; index < scene.samples.size(); ++index) {
        result.samples[index] = static_cast<std::uint16_t>(
            std::lround(std::clamp(static_cast<double>(scene.samples[index]), 0.0, 1.0) * 65'535.0)
        );
    }
    return result;
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
    output << (descriptor.channels == 1U ? "P5\n" : "P6\n") << descriptor.dimensions.width << ' '
           << descriptor.dimensions.height << "\n255\n";
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
        return output_directory
               / (channels == 1U ? "embedded-preview.pgm" : "embedded-preview.ppm");
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
              << "capture.iso=" << metadata.iso_speed << '\n'
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
              << "decoder.capability.reference_rgb=" << (capabilities.reference_rgb ? "yes" : "no")
              << '\n'
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

struct RawFrameCfaSiteStatistics final {
    std::uint64_t sample_count = 0U;
    std::uint64_t below_black = 0U;
    std::uint64_t at_or_above_white = 0U;
    std::uint16_t minimum = std::numeric_limits<std::uint16_t>::max();
    std::uint16_t maximum = 0U;
    double maximum_normalized = -std::numeric_limits<double>::infinity();
    long double total = 0.0L;
};

struct RawFrameInspectionRegion final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

[[nodiscard]] std::uint32_t parse_inspection_coordinate(const char* text, const char* name) {
    const unsigned long value = std::stoul(text);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string(name) + " is outside the uint32 range");
    }
    return static_cast<std::uint32_t>(value);
}

struct RawFrameNormalizationCandidates final {
    std::array<double, 3U> linear_limit_clipped_camera_rgb{};
    std::array<double, 3U> code_max_clipped_camera_rgb{};
    std::array<double, 3U> linear_limit_clipped_srgb{};
    std::array<double, 3U> code_max_clipped_srgb{};
};

[[nodiscard]] std::array<double, 3U> apply_camera_matrix(
    const std::array<double, 9U>& matrix,
    const std::array<double, 3U>& camera_rgb
) {
    return {
        matrix[0] * camera_rgb[0] + matrix[1] * camera_rgb[1] + matrix[2] * camera_rgb[2],
        matrix[3] * camera_rgb[0] + matrix[4] * camera_rgb[1] + matrix[5] * camera_rgb[2],
        matrix[6] * camera_rgb[0] + matrix[7] * camera_rgb[1] + matrix[8] * camera_rgb[2],
    };
}

[[nodiscard]] std::optional<std::size_t> cfa_rgb_index(const image::RawCfaColor color) noexcept {
    switch (color) {
    case image::RawCfaColor::red:
        return 0U;
    case image::RawCfaColor::green:
        return 1U;
    case image::RawCfaColor::blue:
        return 2U;
    case image::RawCfaColor::unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

struct HighlightCfaDeltaSite final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::size_t channel = 0U;
    image::detail::CfaOpposedHighlightSample sample;
};

void render_highlight_cfa_diagnostic(
    image::DecodeSession& session,
    const fs::path& output_directory
) {
    const Stopwatch timer;
    const image::RawFrame frame = session.decode_raw_frame();
    if (!frame.valid() || !frame.is_bayer_2x2()) {
        throw std::runtime_error("CFA highlight diagnostic requires a valid Bayer RAW frame");
    }
    const auto plan = image::preview_raw_development_plan();
    image::RawFrameLinearTransform transform =
        image::raw_pipeline_detail::prepare_raw_frame_linear_transform(
            frame.descriptor,
            plan.white_balance,
            nullptr
        );
    const Stopwatch compiled_chrominance_timer;
    const auto compiled_chrominance_model =
        image::detail::build_opposed_highlight_chrominance_model(frame);
    const auto initial_treatment = image::detail::editable_raw_cfa_sampling_policy(transform);
    const auto compiled_chrominance = image::detail::evaluate_opposed_highlight_chrominance_model(
        compiled_chrominance_model,
        &transform,
        initial_treatment
    );
    transform.opposed_highlight_chrominance_offsets = compiled_chrominance.offsets;
    const double compiled_chrominance_ms = compiled_chrominance_timer.elapsed_ms();
    const auto treatment = image::detail::editable_raw_cfa_sampling_policy(transform);
    const auto chrominance = image::detail::estimate_opposed_highlight_chrominance_correction(
        frame,
        &transform,
        treatment
    );
    const auto sampled_chrominance =
        image::detail::estimate_opposed_highlight_chrominance_correction(
            frame,
            &transform,
            treatment,
            4U
        );
    auto baseline = treatment;
    baseline.reconstruct_terminal_highlights = false;

    std::array<std::uint64_t, 3U> candidates{};
    std::array<std::uint64_t, 3U> raised{};
    std::array<long double, 3U> total_delta{};
    std::array<float, 3U> maximum_delta{};
    std::array<std::array<std::uint64_t, 5U>, 3U> delta_histogram{};
    std::vector<HighlightCfaDeltaSite> largest;
    largest.reserve(512U);
    const auto keep_largest = [&] {
        if (largest.size() <= 256U) {
            return;
        }
        std::nth_element(
            largest.begin(),
            largest.begin() + 256,
            largest.end(),
            [](const HighlightCfaDeltaSite& left, const HighlightCfaDeltaSite& right) {
                return left.sample.reconstructed - left.sample.measured
                       > right.sample.reconstructed - right.sample.measured;
            }
        );
        largest.resize(256U);
    };

    const auto& descriptor = frame.descriptor;
    const std::uint32_t first_x = descriptor.active_margins.left;
    const std::uint32_t first_y = descriptor.active_margins.top;
    const std::uint32_t last_x = first_x + descriptor.active_dimensions.width;
    const std::uint32_t last_y = first_y + descriptor.active_dimensions.height;
    for (std::uint32_t y = first_y; y < last_y; ++y) {
        for (std::uint32_t x = first_x; x < last_x; ++x) {
            const auto channel = cfa_rgb_index(descriptor.bayer_2x2[(y & 1U) * 2U + (x & 1U)]);
            if (!channel.has_value()) {
                continue;
            }
            const auto sample =
                image::detail::opposed_highlight_cfa_sample_at(frame, x, y, &transform, treatment);
            if (!sample.terminal_candidate) {
                continue;
            }
            ++candidates[*channel];
            const float delta = sample.reconstructed - sample.measured;
            const std::size_t bin = delta <= 1.0e-6F  ? 0U
                                    : delta < 1.0e-3F ? 1U
                                    : delta < 1.0e-2F ? 2U
                                    : delta < 5.0e-2F ? 3U
                                                      : 4U;
            ++delta_histogram[*channel][bin];
            if (delta <= 1.0e-6F) {
                continue;
            }
            ++raised[*channel];
            total_delta[*channel] += delta;
            maximum_delta[*channel] = std::max(maximum_delta[*channel], delta);
            largest.push_back(
                HighlightCfaDeltaSite{
                    .x = x,
                    .y = y,
                    .channel = *channel,
                    .sample = sample,
                }
            );
            if (largest.size() >= 512U) {
                keep_largest();
            }
        }
    }
    keep_largest();
    std::sort(
        largest.begin(),
        largest.end(),
        [](const HighlightCfaDeltaSite& left, const HighlightCfaDeltaSite& right) {
            return left.sample.reconstructed - left.sample.measured
                   > right.sample.reconstructed - right.sample.measured;
        }
    );
    if (largest.size() > 64U) {
        largest.resize(64U);
    }
    const fs::path csv_path = output_directory / "highlight-cfa-largest-deltas.csv";
    std::ofstream csv(csv_path);
    if (!csv) {
        throw std::runtime_error("cannot create " + csv_path.string());
    }
    csv << "x,y,cfa,measured,opposed_reference,reconstructed,cfa_delta,"
           "baseline_r,baseline_g,baseline_b,reconstructed_r,reconstructed_g,reconstructed_b,"
           "delta_r,delta_g,delta_b\n";
    std::array<long double, 3U> demosaic_absolute_delta{};
    std::array<float, 3U> demosaic_maximum_delta{};
    long double demosaic_luminance_delta = 0.0L;
    std::uint64_t demosaic_samples = 0U;
    constexpr std::array<const char*, 3U> channel_names{"R", "G", "B"};
    for (const auto& site : largest) {
        const auto before = image::detail::bilinear_camera_rgb_sample_at(
            frame,
            site.x,
            site.y,
            &transform,
            baseline
        );
        const auto after = image::detail::bilinear_camera_rgb_sample_at(
            frame,
            site.x,
            site.y,
            &transform,
            treatment
        );
        std::array<float, 3U> delta{};
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            delta[channel] = after.values[channel] - before.values[channel];
            demosaic_absolute_delta[channel] += std::abs(delta[channel]);
            demosaic_maximum_delta[channel] =
                std::max(demosaic_maximum_delta[channel], std::abs(delta[channel]));
        }
        demosaic_luminance_delta += std::abs(0.25L * delta[0] + 0.5L * delta[1] + 0.25L * delta[2]);
        ++demosaic_samples;
        csv << site.x << ',' << site.y << ',' << channel_names[site.channel] << ','
            << site.sample.measured << ',' << site.sample.opposed_reference << ','
            << site.sample.reconstructed << ',' << site.sample.reconstructed - site.sample.measured
            << ',' << before.values[0] << ',' << before.values[1] << ',' << before.values[2] << ','
            << after.values[0] << ',' << after.values[1] << ',' << after.values[2] << ','
            << delta[0] << ',' << delta[1] << ',' << delta[2] << '\n';
    }
    if (!csv) {
        throw std::runtime_error("cannot write " + csv_path.string());
    }

    std::cout << "highlight_cfa_diagnostic.status=ok\n"
              << "highlight_cfa_diagnostic.algorithm=darktable-opposed-photosite-v1\n"
              << "highlight_cfa_diagnostic.threshold=0.987\n"
              << "highlight_cfa_diagnostic.threshold_domain=physical-white\n"
              << "timing.highlight_cfa_compiled_chrominance_ms=" << compiled_chrominance_ms << '\n';
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        std::cout << "highlight_cfa_diagnostic.channel." << channel_names[channel]
                  << ".chrominance_offset=" << chrominance.offsets[channel] << '\n'
                  << "highlight_cfa_diagnostic.channel." << channel_names[channel]
                  << ".chrominance_support=" << chrominance.supporting_samples[channel] << '\n'
                  << "highlight_cfa_diagnostic.channel." << channel_names[channel]
                  << ".sampled_chrominance_offset=" << sampled_chrominance.offsets[channel] << '\n'
                  << "highlight_cfa_diagnostic.channel." << channel_names[channel]
                  << ".sampled_chrominance_support="
                  << sampled_chrominance.supporting_samples[channel] << '\n'
                  << "highlight_cfa_diagnostic.channel." << channel_names[channel]
                  << ".compiled_chrominance_offset=" << compiled_chrominance.offsets[channel]
                  << '\n'
                  << "highlight_cfa_diagnostic.channel." << channel_names[channel]
                  << ".compiled_chrominance_records="
                  << compiled_chrominance_model.support_records[channel].size() << '\n'
                  << "highlight_cfa_diagnostic.channel." << channel_names[channel]
                  << ".candidates=" << candidates[channel] << '\n'
                  << "highlight_cfa_diagnostic.channel." << channel_names[channel]
                  << ".raised=" << raised[channel] << '\n'
                  << "highlight_cfa_diagnostic.channel." << channel_names[channel]
                  << ".mean_positive_delta="
                  << (raised[channel] == 0U
                          ? 0.0
                          : static_cast<double>(
                                total_delta[channel] / static_cast<long double>(raised[channel])
                            ))
                  << '\n'
                  << "highlight_cfa_diagnostic.channel." << channel_names[channel]
                  << ".maximum_delta=" << maximum_delta[channel] << '\n'
                  << "highlight_cfa_diagnostic.channel." << channel_names[channel]
                  << ".histogram=zero:" << delta_histogram[channel][0]
                  << ",lt0.001:" << delta_histogram[channel][1]
                  << ",lt0.01:" << delta_histogram[channel][2]
                  << ",lt0.05:" << delta_histogram[channel][3]
                  << ",ge0.05:" << delta_histogram[channel][4] << '\n';
    }
    std::cout << "highlight_cfa_diagnostic.demosaic.representative_sites=" << demosaic_samples
              << '\n';
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        std::cout << "highlight_cfa_diagnostic.demosaic.channel." << channel_names[channel]
                  << ".mean_absolute_delta="
                  << (demosaic_samples == 0U ? 0.0
                                             : static_cast<double>(
                                                   demosaic_absolute_delta[channel]
                                                   / static_cast<long double>(demosaic_samples)
                                               ))
                  << '\n'
                  << "highlight_cfa_diagnostic.demosaic.channel." << channel_names[channel]
                  << ".maximum_absolute_delta=" << demosaic_maximum_delta[channel] << '\n';
    }
    std::cout << "highlight_cfa_diagnostic.demosaic.mean_absolute_camera_luminance_delta="
              << (demosaic_samples == 0U
                      ? 0.0
                      : static_cast<double>(
                            demosaic_luminance_delta / static_cast<long double>(demosaic_samples)
                        ))
              << '\n';
    image::probe_detail::render_highlight_cfa_domain_diagnostic(frame, transform, output_directory);
    std::cout << "highlight_cfa_diagnostic.csv=" << csv_path.string() << '\n'
              << "timing.highlight_cfa_diagnostic_ms=" << timer.elapsed_ms() << '\n';
}

void render_highlight_threshold_ablation(
    image::DecodeSession& session,
    const fs::path& output_directory
) {
    const Stopwatch timer;
    const image::RawFrame frame = session.decode_raw_frame();
    if (!frame.valid() || !frame.is_bayer_2x2()) {
        throw std::runtime_error("highlight threshold ablation requires a valid Bayer RAW frame");
    }
    const auto plan = image::preview_raw_development_plan();
    image::RawFrameLinearTransform transform =
        image::raw_pipeline_detail::prepare_raw_frame_linear_transform(
            frame.descriptor,
            plan.white_balance,
            nullptr
        );
    const auto chrominance_model = image::detail::build_opposed_highlight_chrominance_model(frame);
    const auto policy = image::detail::editable_raw_cfa_sampling_policy(transform);
    const auto chrominance = image::detail::evaluate_opposed_highlight_chrominance_model(
        chrominance_model,
        &transform,
        policy
    );
    transform.opposed_highlight_chrominance_offsets = chrominance.offsets;
    image::probe_detail::render_highlight_threshold_ablation(frame, transform, output_directory);
    std::cout << "timing.highlight_threshold_ablation_ms=" << timer.elapsed_ms() << '\n';
}

[[nodiscard]] RawFrameNormalizationCandidates calculate_normalization_candidates(
    const image::RawFrame& frame,
    const std::array<RawFrameCfaSiteStatistics, 4U>& statistics
) {
    if (frame.descriptor.bits_per_sample == 0U || frame.descriptor.bits_per_sample >= 32U) {
        throw std::runtime_error("RAW-frame diagnostic requires a 1..31 bit sensor encoding");
    }
    const auto bounded_mean = [&](const std::size_t site, const std::uint32_t white) {
        const auto& site_statistics = statistics[site];
        const std::uint32_t black = frame.descriptor.black_levels[site];
        if (site_statistics.sample_count == 0U || white <= black
            || !std::isfinite(frame.descriptor.as_shot_neutral[site])
            || frame.descriptor.as_shot_neutral[site] <= 0.0) {
            throw std::runtime_error("RAW-frame diagnostic has incomplete per-site calibration");
        }
        const double mean = static_cast<double>(
            site_statistics.total / static_cast<long double>(site_statistics.sample_count)
        );
        return std::clamp(
            (mean - static_cast<double>(black)) / static_cast<double>(white - black),
            0.0,
            1.0
        );
    };
    const std::uint32_t code_max =
        (std::uint32_t{1} << frame.descriptor.bits_per_sample) - std::uint32_t{1};
    std::array<double, 3U> linear_limit_camera_rgb{};
    std::array<double, 3U> code_max_camera_rgb{};
    std::array<unsigned, 3U> color_counts{};
    for (std::size_t site = 0U; site < statistics.size(); ++site) {
        const auto color = cfa_rgb_index(frame.descriptor.bayer_2x2[site]);
        if (!color.has_value()) {
            throw std::runtime_error(
                "RAW-frame diagnostic encountered an unknown Bayer CFA colour"
            );
        }
        const double white_balance = 1.0 / frame.descriptor.as_shot_neutral[site];
        linear_limit_camera_rgb[*color] +=
            bounded_mean(site, frame.descriptor.white_levels[site]) * white_balance;
        code_max_camera_rgb[*color] += bounded_mean(site, code_max) * white_balance;
        ++color_counts[*color];
    }
    for (std::size_t color = 0U; color < linear_limit_camera_rgb.size(); ++color) {
        if (color_counts[color] == 0U) {
            throw std::runtime_error("Bayer CFA statistics are missing a camera colour channel");
        }
        linear_limit_camera_rgb[color] /= static_cast<double>(color_counts[color]);
        code_max_camera_rgb[color] /= static_cast<double>(color_counts[color]);
    }
    return RawFrameNormalizationCandidates{
        .linear_limit_clipped_camera_rgb = linear_limit_camera_rgb,
        .code_max_clipped_camera_rgb = code_max_camera_rgb,
        .linear_limit_clipped_srgb = apply_camera_matrix(
            frame.descriptor.camera_to_linear_srgb_d65,
            linear_limit_camera_rgb
        ),
        .code_max_clipped_srgb =
            apply_camera_matrix(frame.descriptor.camera_to_linear_srgb_d65, code_max_camera_rgb),
    };
}

void inspect_raw_frame(
    image::DecodeSession& session,
    const fs::path& output_directory,
    const bool write_samples,
    const std::optional<RawFrameInspectionRegion> inspection_region
) {
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

    std::array<RawFrameCfaSiteStatistics, 4U> cfa_site_statistics{};
    if (!write_samples && frame.is_bayer_2x2()) {
        const auto width = frame.descriptor.storage_dimensions.width;
        const auto height = frame.descriptor.storage_dimensions.height;
        const RawFrameInspectionRegion region = inspection_region.value_or(
            RawFrameInspectionRegion{.x = 0U, .y = 0U, .width = width, .height = height}
        );
        if (region.width == 0U || region.height == 0U || region.x >= width || region.y >= height
            || region.width > width - region.x || region.height > height - region.y) {
            throw std::runtime_error("RAW-frame inspection region is outside the stored sensor");
        }
        for (std::uint32_t y = region.y; y < region.y + region.height; ++y) {
            for (std::uint32_t x = region.x; x < region.x + region.width; ++x) {
                const std::size_t site = static_cast<std::size_t>((y & 1U) * 2U + (x & 1U));
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                const std::uint16_t value = frame.samples[index];
                auto& statistics = cfa_site_statistics[site];
                const std::uint32_t black = frame.descriptor.black_levels[site];
                const std::uint32_t white = frame.descriptor.white_levels[site];
                const double normalized = (static_cast<double>(value) - static_cast<double>(black))
                                          / static_cast<double>(white - black);
                ++statistics.sample_count;
                statistics.below_black += value < black ? 1U : 0U;
                statistics.at_or_above_white += value >= white ? 1U : 0U;
                statistics.minimum = std::min(statistics.minimum, value);
                statistics.maximum = std::max(statistics.maximum, value);
                statistics.maximum_normalized = std::max(statistics.maximum_normalized, normalized);
                statistics.total += value;
            }
        }
    }

    const fs::path output_path = output_directory / "raw-frame.pgm";
    if (write_samples) {
        write_u16_pnm(output_path, frame.descriptor.storage_dimensions, 1U, frame.samples);
    }
    const auto sample_count = static_cast<long double>(frame.samples.size());
    const auto& sensor_noise = frame.descriptor.sensor_noise;
    const char* sensor_noise_model =
        sensor_noise.model == image::RawSensorNoiseModel::poisson_gaussian_per_cfa
            ? "poisson-gaussian-per-cfa"
            : "unavailable";
    const char* sensor_noise_source = "unavailable";
    if (sensor_noise.source == image::RawSensorNoiseCalibrationSource::embedded_metadata) {
        sensor_noise_source = "embedded-metadata";
    } else if (
        sensor_noise.source == image::RawSensorNoiseCalibrationSource::provider_calibration_profile
    ) {
        sensor_noise_source = "provider-calibration-profile";
    }

    std::cout << "raw_frame.status=ok\n"
              << "raw_frame.samples=" << frame.samples.size() << '\n'
              << "raw_frame.bits=" << frame.descriptor.bits_per_sample << '\n'
              << "raw_frame.cfa=" << frame.descriptor.cfa_pattern << '\n'
              << "raw_frame.bayer_2x2=" << (frame.is_bayer_2x2() ? "yes" : "no") << '\n'
              << "raw_frame.has_linear_response_limits="
              << (frame.descriptor.has_linear_response_limits ? "yes" : "no") << '\n'
              << "raw_frame.linear_response_limits="
              << frame.descriptor.linear_response_limits[0] << ','
              << frame.descriptor.linear_response_limits[1] << ','
              << frame.descriptor.linear_response_limits[2] << ','
              << frame.descriptor.linear_response_limits[3] << '\n'
              << "raw_frame.as_shot_neutral=" << frame.descriptor.as_shot_neutral[0] << ','
              << frame.descriptor.as_shot_neutral[1] << ',' << frame.descriptor.as_shot_neutral[2]
              << ',' << frame.descriptor.as_shot_neutral[3] << '\n'
              << "raw_frame.camera_to_linear_srgb_d65="
              << frame.descriptor.camera_to_linear_srgb_d65[0] << ','
              << frame.descriptor.camera_to_linear_srgb_d65[1] << ','
              << frame.descriptor.camera_to_linear_srgb_d65[2] << ';'
              << frame.descriptor.camera_to_linear_srgb_d65[3] << ','
              << frame.descriptor.camera_to_linear_srgb_d65[4] << ','
              << frame.descriptor.camera_to_linear_srgb_d65[5] << ';'
              << frame.descriptor.camera_to_linear_srgb_d65[6] << ','
              << frame.descriptor.camera_to_linear_srgb_d65[7] << ','
              << frame.descriptor.camera_to_linear_srgb_d65[8] << '\n'
              << "raw_frame.xyz_to_camera_d65=" << frame.descriptor.xyz_to_camera_d65[0] << ','
              << frame.descriptor.xyz_to_camera_d65[1] << ','
              << frame.descriptor.xyz_to_camera_d65[2] << ';'
              << frame.descriptor.xyz_to_camera_d65[3] << ','
              << frame.descriptor.xyz_to_camera_d65[4] << ','
              << frame.descriptor.xyz_to_camera_d65[5] << ';'
              << frame.descriptor.xyz_to_camera_d65[6] << ','
              << frame.descriptor.xyz_to_camera_d65[7] << ','
              << frame.descriptor.xyz_to_camera_d65[8] << '\n'
              << "raw_frame.sensor_noise.model=" << sensor_noise_model << '\n'
              << "raw_frame.sensor_noise.source=" << sensor_noise_source << '\n'
              << "raw_frame.sensor_noise.iso=" << sensor_noise.iso_sensitivity << '\n'
              << "raw_frame.sensor_noise.read_stddev_dn=" << sensor_noise.read_noise_stddev_dn[0]
              << ',' << sensor_noise.read_noise_stddev_dn[1] << ','
              << sensor_noise.read_noise_stddev_dn[2] << ',' << sensor_noise.read_noise_stddev_dn[3]
              << '\n'
              << "raw_frame.sensor_noise.shot_variance_per_dn="
              << sensor_noise.shot_noise_variance_per_dn[0] << ','
              << sensor_noise.shot_noise_variance_per_dn[1] << ','
              << sensor_noise.shot_noise_variance_per_dn[2] << ','
              << sensor_noise.shot_noise_variance_per_dn[3] << '\n'
              << "raw_frame.minimum=" << *minimum << '\n'
              << "raw_frame.maximum=" << *maximum << '\n'
              << "raw_frame.mean=" << static_cast<double>(total / sample_count) << '\n'
              << "raw_frame.fnv1a64=" << std::hex << std::setw(16) << std::setfill('0') << checksum
              << std::dec << std::setfill(' ') << '\n'
              << "raw_frame.output=" << (write_samples ? output_path.string() : "not-written")
              << '\n'
              << "timing.raw_frame_ms=" << timer.elapsed_ms() << '\n';
    if (!write_samples && frame.is_bayer_2x2()) {
        const RawFrameInspectionRegion region = inspection_region.value_or(
            RawFrameInspectionRegion{
                .x = 0U,
                .y = 0U,
                .width = frame.descriptor.storage_dimensions.width,
                .height = frame.descriptor.storage_dimensions.height,
            }
        );
        std::cout << "raw_frame.inspection_region=" << region.x << ',' << region.y << ','
                  << region.width << 'x' << region.height << '\n';
        for (std::size_t site = 0U; site < cfa_site_statistics.size(); ++site) {
            const auto& statistics = cfa_site_statistics[site];
            const double mean = static_cast<double>(
                statistics.total / static_cast<long double>(statistics.sample_count)
            );
            const std::uint32_t code_max =
                (std::uint32_t{1} << frame.descriptor.bits_per_sample) - std::uint32_t{1};
            const std::uint32_t black = frame.descriptor.black_levels[site];
            const double mean_normalized_linear_limit =
                (mean - static_cast<double>(black))
                / static_cast<double>(frame.descriptor.white_levels[site] - black);
            const double mean_normalized_code_max =
                (mean - static_cast<double>(black)) / static_cast<double>(code_max - black);
            std::cout << "raw_frame.cfa_site." << site
                      << ".color=" << static_cast<unsigned>(frame.descriptor.bayer_2x2[site])
                      << '\n'
                      << "raw_frame.cfa_site." << site
                      << ".black=" << frame.descriptor.black_levels[site] << '\n'
                      << "raw_frame.cfa_site." << site
                      << ".white=" << frame.descriptor.white_levels[site] << '\n'
                      << "raw_frame.cfa_site." << site << ".minimum=" << statistics.minimum << '\n'
                      << "raw_frame.cfa_site." << site << ".maximum=" << statistics.maximum << '\n'
                      << "raw_frame.cfa_site." << site << ".mean=" << mean << '\n'
                      << "raw_frame.cfa_site." << site
                      << ".mean_normalized_linear_limit_unbounded=" << mean_normalized_linear_limit
                      << '\n'
                      << "raw_frame.cfa_site." << site
                      << ".mean_normalized_code_max=" << mean_normalized_code_max << '\n'
                      << "raw_frame.cfa_site." << site
                      << ".max_normalized=" << statistics.maximum_normalized << '\n'
                      << "raw_frame.cfa_site." << site << ".below_black=" << statistics.below_black
                      << '\n'
                      << "raw_frame.cfa_site." << site
                      << ".at_or_above_white=" << statistics.at_or_above_white << '\n';
        }
        const RawFrameNormalizationCandidates candidates =
            calculate_normalization_candidates(frame, cfa_site_statistics);
        std::cout << "raw_frame.candidate.note=CFA-site-mean diagnostic only; not a demosaic or "
                     "rendering prediction\n"
                  << "raw_frame.candidate.rendering_path=unchanged\n"
                  << "raw_frame.candidate.linear_limit_clipped.camera_rgb="
                  << candidates.linear_limit_clipped_camera_rgb[0] << ','
                  << candidates.linear_limit_clipped_camera_rgb[1] << ','
                  << candidates.linear_limit_clipped_camera_rgb[2] << '\n'
                  << "raw_frame.candidate.linear_limit_clipped.linear_srgb="
                  << candidates.linear_limit_clipped_srgb[0] << ','
                  << candidates.linear_limit_clipped_srgb[1] << ','
                  << candidates.linear_limit_clipped_srgb[2] << '\n'
                  << "raw_frame.candidate.code_max_clipped.camera_rgb="
                  << candidates.code_max_clipped_camera_rgb[0] << ','
                  << candidates.code_max_clipped_camera_rgb[1] << ','
                  << candidates.code_max_clipped_camera_rgb[2] << '\n'
                  << "raw_frame.candidate.code_max_clipped.linear_srgb="
                  << candidates.code_max_clipped_srgb[0] << ','
                  << candidates.code_max_clipped_srgb[1] << ','
                  << candidates.code_max_clipped_srgb[2] << '\n';
    }
}

void render_reference_rgb(
    image::DecodeSession& session,
    const fs::path& output_directory,
    const std::optional<image::RawWhiteBalance> white_balance
) {
    const Stopwatch timer;
    auto plan = image::default_raw_development_plan();
    if (white_balance.has_value()) {
        plan.white_balance = *white_balance;
    }
    const image::DevelopedSourceReference source = image::develop_source_reference(
        session,
        plan,
        std::nullopt,
        image::raw_pipeline_policy_from_environment()
    );
    const ProbeLinearSource rendered = materialize_probe_source(source.source);
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
              << source.raw_development_receipt.requested_plan_identity << '\n'
              << "reference_rgb.plan.effective="
              << source.raw_development_receipt.effective_plan_identity << '\n'
              << "reference_rgb.development="
              << source.raw_development_receipt.development_settings_signature << '\n'
              << "reference_rgb.storage="
              << (rendered.scene_linear_f32 ? "scene-linear-f32" : "packed-u16") << '\n'
              << "reference_rgb.pipeline.path="
              << static_cast<unsigned>(source.pipeline_receipt.path) << '\n'
              << "reference_rgb.pipeline.identity="
              << image::raw_pipeline_receipt_identity(source.pipeline_receipt) << '\n'
              << "reference_rgb.output=" << output_path.string() << '\n'
              << "timing.reference_rgb_ms=" << timer.elapsed_ms() << '\n';
}

void render_warm_preview_reference_rgb(
    image::DecodeSession& session,
    const fs::path& output_directory,
    const std::optional<image::RawWhiteBalance> white_balance
) {
    constexpr std::uint32_t warm_preview_edge = 1'200U;
    const Stopwatch timer;
    auto plan = image::preview_raw_development_plan();
    if (white_balance.has_value()) {
        plan.white_balance = *white_balance;
    }
    const image::DevelopedSourceReference source = image::develop_source_reference(
        session,
        plan,
        warm_preview_edge,
        image::raw_pipeline_policy_from_environment()
    );
    const ProbeLinearSource rendered = materialize_probe_source(source.source);
    const fs::path output_path = output_directory / "preview-reference-linear-srgb-16bit.ppm";
    write_u16_pnm(output_path, rendered.dimensions, rendered.channels, rendered.samples);
    std::array<long double, 3U> channel_sum{};
    const std::size_t pixel_count =
        static_cast<std::size_t>(rendered.dimensions.width) * rendered.dimensions.height;
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
              << source.raw_development_receipt.requested_plan_identity << '\n'
              << "preview_reference.plan.effective="
              << source.raw_development_receipt.effective_plan_identity << '\n'
              << "preview_reference.pipeline.path="
              << static_cast<unsigned>(source.pipeline_receipt.path) << '\n'
              << "preview_reference.pipeline.identity="
              << image::raw_pipeline_receipt_identity(source.pipeline_receipt) << '\n'
              << "preview_reference.linear_mean_rgb="
              << static_cast<double>(channel_sum[0] / static_cast<long double>(pixel_count)) << ','
              << static_cast<double>(channel_sum[1] / static_cast<long double>(pixel_count)) << ','
              << static_cast<double>(channel_sum[2] / static_cast<long double>(pixel_count)) << '\n'
              << "preview_reference.output=" << output_path.string() << '\n'
              << "timing.preview_reference_ms=" << timer.elapsed_ms() << '\n';
}

[[nodiscard]] double display_high_frequency_energy(const image::DisplayLumaImage& luma) {
    if (luma.dimensions.width < 3U || luma.dimensions.height < 3U) {
        return 0.0;
    }
    long double total = 0.0L;
    std::uint64_t sample_count = 0U;
    for (std::uint32_t y = 1U; y + 1U < luma.dimensions.height; ++y) {
        for (std::uint32_t x = 1U; x + 1U < luma.dimensions.width; ++x) {
            const std::size_t center = static_cast<std::size_t>(y) * luma.row_stride_samples + x;
            const double neighbourhood = (luma.samples[center - 1U] + luma.samples[center + 1U]
                                          + luma.samples[center - luma.row_stride_samples]
                                          + luma.samples[center + luma.row_stride_samples])
                                         * 0.25;
            total += std::abs(static_cast<double>(luma.samples[center]) - neighbourhood);
            ++sample_count;
        }
    }
    return sample_count == 0U ? 0.0
                              : static_cast<double>(total / static_cast<long double>(sample_count));
}

void render_warm_denoise_diagnostic(
    const image::DecodeSession& session,
    const fs::path& output_directory
) {
    // Match the desktop's current diagnostic case rather than testing an isolated image kernel:
    // a 1536px warm edit source, its complete CPU fallback for non-local operations, JPEG output,
    // and the same Detail & Effects node representation that is persisted in the catalog.
    constexpr std::uint32_t warm_preview_edge = 1'536U;
    const image::WarmEditPreviewSession preview =
        image::prepare_warm_edit_preview(session, warm_preview_edge);
    const std::array<image::AdjustmentNode, 0U> neutral_nodes{};
    const std::array denoise_nodes{
        image::AdjustmentNode{
            .node_id = "raw-probe-max-denoise",
            .parameter_schema_version = image::detail_effects_parameter_schema_version,
            .implementation_version = image::technical_detail_implementation_version,
            .parameters = image::SharpenAdjustment{
                .denoise_luminance = 1.0,
                .denoise_detail = 0.24,
                .denoise_color = 1.0,
            },
        },
    };

    const Stopwatch timer;
    const image::AnalyzedEditPreview neutral = preview.render_jpeg_with_analysis(neutral_nodes);
    const double neutral_ms = timer.elapsed_ms();
    const image::AnalyzedEditPreview denoised = preview.render_jpeg_with_analysis(denoise_nodes);
    const double denoised_ms = timer.elapsed_ms() - neutral_ms;
    auto robust_raw_plan = image::preview_raw_development_plan();
    robust_raw_plan.noise_reduction = image::RawNoiseReductionIntent::noise_robust;
    const Stopwatch raw_denoise_timer;
    const image::WarmEditPreviewSession raw_denoised_preview =
        image::prepare_warm_edit_preview(session, warm_preview_edge, robust_raw_plan);
    const double raw_denoise_prepare_ms = raw_denoise_timer.elapsed_ms();
    const image::AnalyzedEditPreview raw_denoised =
        raw_denoised_preview.render_jpeg_with_analysis(neutral_nodes);
    const double raw_denoise_render_ms = raw_denoise_timer.elapsed_ms() - raw_denoise_prepare_ms;
    write_binary(output_directory / "warm-neutral.jpg", neutral.proxy.bytes);
    write_binary(output_directory / "warm-denoise-l100-d24-c100.jpg", denoised.proxy.bytes);
    write_binary(output_directory / "warm-raw-denoise-robust.jpg", raw_denoised.proxy.bytes);

    const image::DisplayLumaImage neutral_luma =
        image::decode_jpeg_display_luma(neutral.proxy.bytes);
    const image::DisplayLumaImage denoised_luma =
        image::decode_jpeg_display_luma(denoised.proxy.bytes);
    const image::DisplayLumaImage raw_denoised_luma =
        image::decode_jpeg_display_luma(raw_denoised.proxy.bytes);
    if (neutral_luma.dimensions != denoised_luma.dimensions
        || neutral_luma.dimensions != raw_denoised_luma.dimensions
        || neutral_luma.samples.size() != denoised_luma.samples.size()
        || neutral_luma.samples.size() != raw_denoised_luma.samples.size()) {
        throw std::runtime_error("warm denoise diagnostic produced mismatched output dimensions");
    }
    long double absolute_difference = 0.0L;
    long double raw_absolute_difference = 0.0L;
    for (std::size_t index = 0U; index < neutral_luma.samples.size(); ++index) {
        absolute_difference += std::abs(
            static_cast<double>(neutral_luma.samples[index])
            - static_cast<double>(denoised_luma.samples[index])
        );
        raw_absolute_difference += std::abs(
            static_cast<double>(neutral_luma.samples[index])
            - static_cast<double>(raw_denoised_luma.samples[index])
        );
    }
    const double neutral_energy = display_high_frequency_energy(neutral_luma);
    const double denoised_energy = display_high_frequency_energy(denoised_luma);
    const double raw_denoised_energy = display_high_frequency_energy(raw_denoised_luma);
    const auto backend_name = [](const image::EditPreviewBackend backend) {
        return backend == image::EditPreviewBackend::metal ? "metal" : "cpu";
    };
    std::cout << "denoise_diagnostic.status=ok\n"
              << "denoise_diagnostic.dimensions=" << neutral.proxy.dimensions.width << 'x'
              << neutral.proxy.dimensions.height << '\n'
              << "denoise_diagnostic.parameters=luminance:1.000,detail:0.240,color:1.000\n"
              << "denoise_diagnostic.execution.neutral="
              << backend_name(neutral.execution.adjustment_backend) << '\n'
              << "denoise_diagnostic.execution.denoised="
              << backend_name(denoised.execution.adjustment_backend) << '\n'
              << "denoise_diagnostic.display_mean_absolute_difference="
              << static_cast<double>(
                     absolute_difference / static_cast<long double>(neutral_luma.samples.size())
                 )
              << '\n'
              << "denoise_diagnostic.raw_robust_display_mean_absolute_difference="
              << static_cast<double>(
                     raw_absolute_difference / static_cast<long double>(neutral_luma.samples.size())
                 )
              << '\n'
              << "denoise_diagnostic.display_high_frequency_energy.neutral=" << neutral_energy
              << '\n'
              << "denoise_diagnostic.display_high_frequency_energy.denoised=" << denoised_energy
              << '\n'
              << "denoise_diagnostic.display_high_frequency_energy_reduction="
              << (neutral_energy == 0.0 ? 0.0 : 1.0 - denoised_energy / neutral_energy) << '\n'
              << "denoise_diagnostic.raw_robust_high_frequency_energy=" << raw_denoised_energy
              << '\n'
              << "denoise_diagnostic.raw_robust_high_frequency_energy_reduction="
              << (neutral_energy == 0.0 ? 0.0 : 1.0 - raw_denoised_energy / neutral_energy) << '\n'
              << "denoise_diagnostic.raw_robust_development="
              << raw_denoised_preview.raw_development_receipt().development_settings_signature
              << '\n'
              << "denoise_diagnostic.neutral_output="
              << (output_directory / "warm-neutral.jpg").string() << '\n'
              << "denoise_diagnostic.denoised_output="
              << (output_directory / "warm-denoise-l100-d24-c100.jpg").string() << '\n'
              << "denoise_diagnostic.raw_robust_output="
              << (output_directory / "warm-raw-denoise-robust.jpg").string() << '\n'
              << "timing.denoise_diagnostic.neutral_ms=" << neutral_ms << '\n'
              << "timing.denoise_diagnostic.denoised_ms=" << denoised_ms << '\n'
              << "timing.denoise_diagnostic.raw_robust_prepare_ms=" << raw_denoise_prepare_ms
              << '\n'
              << "timing.denoise_diagnostic.raw_robust_render_ms=" << raw_denoise_render_ms << '\n';
}

void render_warm_highlight_diagnostic(
    const image::DecodeSession& session,
    const fs::path& output_directory
) {
    constexpr std::uint32_t warm_preview_edge = 1'536U;
    const std::array<image::AdjustmentNode, 0U> neutral_nodes{};
    const std::array recovery_nodes{
        image::AdjustmentNode{
            .node_id = "highlight-diagnostic-recovery",
            .parameter_schema_version = image::selective_tone_parameter_schema_version,
            .implementation_version = image::selective_tone_implementation_version,
            .parameters = image::SelectiveToneAdjustment{
                .highlights = -1.0,
                .whites = -1.0,
            },
        },
    };
    auto enabled_plan = image::preview_raw_development_plan();
    auto disabled_plan = enabled_plan;
    disabled_plan.highlight_recovery = image::RawHighlightRecoveryIntent::disabled;
    auto aggressive_plan = enabled_plan;
    aggressive_plan.highlight_recovery = image::RawHighlightRecoveryIntent::aggressive;
    const image::WarmEditPreviewSession enabled =
        image::prepare_warm_edit_preview(session, warm_preview_edge, enabled_plan);
    const image::WarmEditPreviewSession disabled =
        image::prepare_warm_edit_preview(session, warm_preview_edge, disabled_plan);
    const image::WarmEditPreviewSession aggressive =
        image::prepare_warm_edit_preview(session, warm_preview_edge, aggressive_plan);
    const image::AnalyzedEditPreview enabled_preview =
        enabled.render_jpeg_with_analysis(neutral_nodes);
    const image::AnalyzedEditPreview disabled_preview =
        disabled.render_jpeg_with_analysis(neutral_nodes);
    const image::AnalyzedEditPreview default_recovered =
        enabled.render_jpeg_with_analysis(recovery_nodes);
    const image::AnalyzedEditPreview disabled_recovered =
        disabled.render_jpeg_with_analysis(recovery_nodes);
    const image::AnalyzedEditPreview aggressive_recovered =
        aggressive.render_jpeg_with_analysis(recovery_nodes);
    write_binary(output_directory / "warm-highlight-default.jpg", enabled_preview.proxy.bytes);
    write_binary(output_directory / "warm-highlight-disabled.jpg", disabled_preview.proxy.bytes);
    write_binary(
        output_directory / "warm-highlight-default-recovered.jpg",
        default_recovered.proxy.bytes
    );
    write_binary(
        output_directory / "warm-highlight-disabled-recovered.jpg",
        disabled_recovered.proxy.bytes
    );
    write_binary(
        output_directory / "warm-highlight-aggressive-recovered.jpg",
        aggressive_recovered.proxy.bytes
    );
    fs::path shared_coverage_path;
    if (const auto& clipping = enabled.sensor_clipping_mask(); clipping.has_value()) {
        std::vector<std::uint8_t> coverage(clipping->samples.size());
        for (std::size_t pixel = 0U; pixel < coverage.size(); ++pixel) {
            coverage[pixel] = static_cast<std::uint8_t>(
                std::lround(clipping->shared_highlight_coverage_at(pixel) * 255.0F)
            );
        }
        shared_coverage_path = output_directory / "warm-highlight-shared-coverage.pgm";
        write_u8_pgm(shared_coverage_path, clipping->dimensions, coverage);
    }
    fs::path grade_risk_path;
    std::uint64_t grade_risk_nonzero_pixels = 0U;
    std::uint8_t grade_risk_maximum = 0U;
    if (const auto& risk = enabled.highlight_chroma_risk_map(); risk.has_value()) {
        grade_risk_path = output_directory / "warm-highlight-grade-risk.pgm";
        write_u8_pgm(grade_risk_path, risk->dimensions, risk->samples);
        for (const auto sample : risk->samples) {
            grade_risk_nonzero_pixels += sample != 0U ? 1U : 0U;
            grade_risk_maximum = std::max(grade_risk_maximum, sample);
        }
    }
    std::cout << "highlight_diagnostic.status=ok\n"
              << "highlight_diagnostic.default_output="
              << (output_directory / "warm-highlight-default.jpg").string() << '\n'
              << "highlight_diagnostic.disabled_output="
              << (output_directory / "warm-highlight-disabled.jpg").string() << '\n'
              << "highlight_diagnostic.default_recovered_output="
              << (output_directory / "warm-highlight-default-recovered.jpg").string() << '\n'
              << "highlight_diagnostic.disabled_recovered_output="
              << (output_directory / "warm-highlight-disabled-recovered.jpg").string() << '\n'
              << "highlight_diagnostic.aggressive_recovered_output="
              << (output_directory / "warm-highlight-aggressive-recovered.jpg").string() << '\n'
              << "highlight_diagnostic.shared_coverage_output=" << shared_coverage_path.string()
              << '\n'
              << "highlight_diagnostic.grade_risk_output=" << grade_risk_path.string() << '\n'
              << "highlight_diagnostic.grade_risk_nonzero_pixels="
              << grade_risk_nonzero_pixels << '\n'
              << "highlight_diagnostic.grade_risk_maximum="
              << static_cast<unsigned int>(grade_risk_maximum)
              << '\n';
}

int run(
    const fs::path& input_path,
    const fs::path& output_directory,
    const bool preview_only,
    const bool denoise_diagnostic,
    const bool highlight_diagnostic,
    const bool highlight_cfa_diagnostic,
    const bool highlight_threshold_ablation,
    const bool raw_frame_only,
    const bool raw_frame_statistics_only,
    const std::optional<RawFrameInspectionRegion> raw_frame_inspection_region,
    const std::optional<image::RawWhiteBalance> white_balance
) {
    fs::create_directories(output_directory);
    // Deliberately use the same routed provider as the desktop app. With no
    // SHADOW_PRIVATE_DECODER_PLUGIN_PATH it resolves to the public LibRaw
    // implementation; with one configured it exercises a private adapter's
    // complete open/metadata/preview/RAW-frame path before falling back.
    const auto provider = image::make_photo_decoder_provider();

    const Stopwatch open_timer;
    const auto session = provider->open(input_path);
    std::cout << std::fixed << std::setprecision(3) << "input=" << input_path.string() << '\n'
              << "output_directory=" << output_directory.string() << '\n'
              << "timing.open_ms=" << open_timer.elapsed_ms() << '\n';
    print_session(provider->info(), *session);
    if (highlight_cfa_diagnostic) {
        render_highlight_cfa_diagnostic(*session, output_directory);
        return 0;
    }
    if (highlight_threshold_ablation) {
        render_highlight_threshold_ablation(*session, output_directory);
        return 0;
    }
    if (raw_frame_only || raw_frame_statistics_only) {
        if (!session->capabilities().raw_frame) {
            std::cout << "raw_frame.status=unavailable\n"
                      << "raw_frame.reason=provider-does-not-expose-raw-frame\n";
            return 0;
        }
        inspect_raw_frame(
            *session,
            output_directory,
            raw_frame_only,
            raw_frame_statistics_only ? raw_frame_inspection_region : std::nullopt
        );
        return 0;
    }
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
    render_warm_preview_reference_rgb(*session, output_directory, white_balance);
    if (highlight_diagnostic) {
        render_warm_highlight_diagnostic(*session, output_directory);
        return 0;
    }
    if (denoise_diagnostic) {
        render_warm_denoise_diagnostic(*session, output_directory);
        return 0;
    }
    if (preview_only) {
        return 0;
    }
    if (!session->capabilities().raw_frame) {
        std::cout << "raw_frame.status=unavailable\n"
                  << "raw_frame.reason=provider-does-not-expose-raw-frame\n";
        render_reference_rgb(*session, output_directory, white_balance);
        return 0;
    }
    inspect_raw_frame(*session, output_directory, true, std::nullopt);
    render_reference_rgb(*session, output_directory, white_balance);
    return 0;
}

} // namespace

int main(const int argument_count, char** arguments) {
    const bool preview_only =
        argument_count == 4 && std::string_view(arguments[3]) == "--preview-only";
    const bool denoise_diagnostic =
        argument_count == 4 && std::string_view(arguments[3]) == "--denoise-diagnostic";
    const bool highlight_diagnostic =
        argument_count == 4 && std::string_view(arguments[3]) == "--highlight-diagnostic";
    const bool highlight_cfa_diagnostic =
        argument_count == 4 && std::string_view(arguments[3]) == "--highlight-cfa-diagnostic";
    const bool highlight_threshold_ablation =
        argument_count == 4 && std::string_view(arguments[3]) == "--highlight-threshold-ablation";
    const bool raw_frame_only =
        argument_count == 4 && std::string_view(arguments[3]) == "--raw-frame-only";
    const bool raw_frame_statistics_only =
        argument_count == 4 && std::string_view(arguments[3]) == "--raw-frame-stats";
    const bool raw_frame_statistics_region =
        argument_count == 8 && std::string_view(arguments[3]) == "--raw-frame-stats-region";
    const bool manual_white_balance =
        argument_count == 6 && std::string_view(arguments[3]) == "--manual-white-balance";
    if (argument_count != 3 && !preview_only && !denoise_diagnostic && !highlight_diagnostic
        && !highlight_cfa_diagnostic && !raw_frame_only && !raw_frame_statistics_only
        && !highlight_threshold_ablation && !raw_frame_statistics_region && !manual_white_balance) {
        std::cerr << "usage: shadow-raw-probe <input-raw> <output-directory> "
                     "[--preview-only|--denoise-diagnostic|--highlight-diagnostic|"
                     "--highlight-cfa-diagnostic|"
                     "--highlight-threshold-ablation|"
                     "--raw-frame-only|--raw-frame-stats|"
                     "--raw-frame-stats-region <x> <y> <width> <height>|"
                     "--manual-white-balance <kelvin> <tint>]\n";
        return 2;
    }

    try {
        std::optional<RawFrameInspectionRegion> raw_frame_inspection_region;
        if (raw_frame_statistics_region) {
            raw_frame_inspection_region = RawFrameInspectionRegion{
                .x = parse_inspection_coordinate(arguments[4], "inspection x"),
                .y = parse_inspection_coordinate(arguments[5], "inspection y"),
                .width = parse_inspection_coordinate(arguments[6], "inspection width"),
                .height = parse_inspection_coordinate(arguments[7], "inspection height"),
            };
        }
        std::optional<image::RawWhiteBalance> white_balance;
        if (manual_white_balance) {
            const auto temperature = std::stoul(arguments[4]);
            const auto tint = std::stol(arguments[5]);
            if (temperature > std::numeric_limits<std::uint32_t>::max()
                || tint < std::numeric_limits<std::int16_t>::min()
                || tint > std::numeric_limits<std::int16_t>::max()) {
                throw std::invalid_argument("manual white balance is outside its integer range");
            }
            white_balance = image::RawWhiteBalance{
                .mode = image::RawWhiteBalanceMode::temperature_tint,
                .temperature_kelvin = static_cast<std::uint32_t>(temperature),
                .tint = static_cast<std::int16_t>(tint),
            };
            if (!image::valid_raw_white_balance(*white_balance)) {
                throw std::invalid_argument("manual white balance is outside the supported range");
            }
        }
        return run(
            arguments[1],
            arguments[2],
            preview_only,
            denoise_diagnostic,
            highlight_diagnostic,
            highlight_cfa_diagnostic,
            highlight_threshold_ablation,
            raw_frame_only,
            raw_frame_statistics_only || raw_frame_statistics_region,
            raw_frame_inspection_region,
            white_balance
        );
    } catch (const image::DecodeError& error) {
        std::cerr << "shadow-raw-probe: " << error.what() << " [provider=" << error.provider_code()
                  << "]\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "shadow-raw-probe: " << error.what() << '\n';
        return 1;
    }
}

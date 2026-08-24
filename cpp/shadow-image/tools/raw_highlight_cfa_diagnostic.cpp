#include "raw_highlight_cfa_diagnostic.hpp"
#include "raw_highlight_reference_pipeline.hpp"

#include "../src/raw/bayer_sampling.hpp"

#include <shadow/image/fused_raw_development.hpp>
#include <shadow/image/raw_frame.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace shadow::image::probe_detail {
namespace {

struct DomainDeltaSite final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::size_t channel = 0U;
    detail::CfaOpposedHighlightSample common_white;
    detail::CfaOpposedHighlightSample darktable_domain;
};

struct ProfileRow final {
    std::uint32_t x = 0U;
    std::size_t channel = 0U;
    detail::CfaOpposedHighlightSample common_white;
    detail::CfaOpposedHighlightSample darktable_domain;
    std::array<double, 3U> common_scene_rgb{};
    std::array<double, 3U> darktable_scene_rgb{};
    double common_scene_luminance = 0.0;
    double darktable_scene_luminance = 0.0;
};

[[nodiscard]] std::optional<std::size_t> cfa_rgb_index(const RawCfaColor color) noexcept {
    switch (color) {
    case RawCfaColor::red:
        return 0U;
    case RawCfaColor::green:
        return 1U;
    case RawCfaColor::blue:
        return 2U;
    case RawCfaColor::unknown:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::array<double, 3U> camera_sample_to_scene_rgb(
    const RawFrameLinearTransform& transform,
    const detail::CameraRgbSample& sample
) noexcept {
    std::array<double, 3U> result{};
    for (std::size_t output = 0U; output < result.size(); ++output) {
        for (std::size_t input = 0U; input < sample.values.size(); ++input) {
            result[output] += transform.camera_to_linear_srgb_d65[output * 3U + input]
                              * static_cast<double>(sample.values[input]);
        }
    }
    return result;
}

[[nodiscard]] double linear_srgb_luminance(const std::array<double, 3U>& rgb) noexcept {
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
}

[[nodiscard]] float domain_difference(const DomainDeltaSite& site) noexcept {
    return std::abs(site.darktable_domain.reconstructed - site.common_white.reconstructed);
}

void retain_largest_domain_differences(std::vector<DomainDeltaSite>& sites) {
    constexpr std::size_t retained_count = 256U;
    if (sites.size() <= retained_count) {
        return;
    }
    std::nth_element(
        sites.begin(),
        sites.begin() + retained_count,
        sites.end(),
        [](const DomainDeltaSite& left, const DomainDeltaSite& right) {
            return domain_difference(left) > domain_difference(right);
        }
    );
    sites.resize(retained_count);
}

[[nodiscard]] std::vector<ProfileRow> make_horizontal_profile(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const detail::BayerCfaSamplingPolicy common_treatment,
    const detail::BayerCfaSamplingPolicy darktable_treatment,
    const std::uint32_t y
) {
    const auto& descriptor = frame.descriptor;
    const std::uint32_t first_x = descriptor.active_margins.left;
    const std::uint32_t last_x = first_x + descriptor.active_dimensions.width;
    std::vector<ProfileRow> profile;
    profile.reserve(descriptor.active_dimensions.width);
    for (std::uint32_t x = first_x; x < last_x; ++x) {
        const auto channel = cfa_rgb_index(descriptor.bayer_2x2[(y & 1U) * 2U + (x & 1U)]);
        if (!channel.has_value()) {
            continue;
        }
        const auto common_site =
            detail::opposed_highlight_cfa_sample_at(frame, x, y, &transform, common_treatment);
        const auto darktable_site =
            detail::opposed_highlight_cfa_sample_at(frame, x, y, &transform, darktable_treatment);
        const auto common_rgb =
            detail::bilinear_camera_rgb_sample_at(frame, x, y, &transform, common_treatment);
        const auto darktable_rgb =
            detail::bilinear_camera_rgb_sample_at(frame, x, y, &transform, darktable_treatment);
        const auto common_scene = camera_sample_to_scene_rgb(transform, common_rgb);
        const auto darktable_scene = camera_sample_to_scene_rgb(transform, darktable_rgb);
        profile.push_back(
            ProfileRow{
                .x = x,
                .channel = *channel,
                .common_white = common_site,
                .darktable_domain = darktable_site,
                .common_scene_rgb = common_scene,
                .darktable_scene_rgb = darktable_scene,
                .common_scene_luminance = linear_srgb_luminance(common_scene),
                .darktable_scene_luminance = linear_srgb_luminance(darktable_scene),
            }
        );
    }
    return profile;
}

struct ProfileDerivativeStatistics final {
    double common_first_maximum = 0.0;
    double darktable_first_maximum = 0.0;
    double common_second_maximum = 0.0;
    double darktable_second_maximum = 0.0;
};

[[nodiscard]] Dimensions
reference_preview_dimensions(const Dimensions source, const std::uint32_t maximum_edge) noexcept {
    if (source.width <= maximum_edge && source.height <= maximum_edge) {
        return source;
    }
    const double scale = static_cast<double>(maximum_edge)
                         / static_cast<double>(std::max(source.width, source.height));
    return Dimensions{
        .width = std::max(1U, static_cast<std::uint32_t>(std::lround(source.width * scale))),
        .height = std::max(1U, static_cast<std::uint32_t>(std::lround(source.height * scale))),
    };
}

[[nodiscard]] std::array<float, 3U> camera_rgb_to_scene_rgb(
    const RawFrameLinearTransform& transform,
    const std::array<float, 3U>& camera_rgb
) noexcept {
    std::array<float, 3U> result{};
    for (std::size_t output = 0U; output < result.size(); ++output) {
        double value = 0.0;
        for (std::size_t input = 0U; input < camera_rgb.size(); ++input) {
            value += transform.camera_to_linear_srgb_d65[output * 3U + input]
                     * static_cast<double>(camera_rgb[input]);
        }
        result[output] = static_cast<float>(value);
    }
    return result;
}

void write_float_pfm(
    const std::filesystem::path& path,
    const Dimensions dimensions,
    const std::vector<float>& samples
) {
    if (samples.size() != static_cast<std::size_t>(dimensions.pixel_count()) * 3U) {
        throw std::runtime_error("invalid float RGB plane for " + path.string());
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot create " + path.string());
    }
    output << "PF\n" << dimensions.width << ' ' << dimensions.height << "\n-1.0\n";
    for (std::uint32_t row = dimensions.height; row > 0U; --row) {
        const auto begin =
            samples.data() + static_cast<std::size_t>(row - 1U) * dimensions.width * 3U;
        output.write(
            reinterpret_cast<const char*>(begin),
            static_cast<std::streamsize>(dimensions.width * 3U * sizeof(float))
        );
    }
    if (!output) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

[[nodiscard]] float display_encode(const float linear) noexcept {
    const float positive = std::max(0.0F, linear);
    const float compressed = positive / (1.0F + positive);
    return compressed <= 0.0031308F ? 12.92F * compressed
                                    : 1.055F * std::pow(compressed, 1.0F / 2.4F) - 0.055F;
}

void write_display_ppm(
    const std::filesystem::path& path,
    const Dimensions dimensions,
    const std::vector<float>& samples,
    const float linear_scale = 1.0F
) {
    if (samples.size() != static_cast<std::size_t>(dimensions.pixel_count()) * 3U) {
        throw std::runtime_error("invalid display RGB plane for " + path.string());
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("cannot create " + path.string());
    }
    output << "P6\n" << dimensions.width << ' ' << dimensions.height << "\n255\n";
    std::array<char, 3U> pixel{};
    for (std::size_t offset = 0U; offset < samples.size(); offset += 3U) {
        for (std::size_t channel = 0U; channel < pixel.size(); ++channel) {
            const float encoded = display_encode(samples[offset + channel] * linear_scale);
            pixel[channel] =
                static_cast<char>(std::lround(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
        }
        output.write(pixel.data(), static_cast<std::streamsize>(pixel.size()));
    }
    if (!output) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

struct ReferenceRenderOutputs final {
    std::filesystem::path shadow_pfm;
    std::filesystem::path darktable_pfm;
    std::filesystem::path shadow_ppm;
    std::filesystem::path darktable_ppm;
    std::filesystem::path difference_ppm;
    Dimensions dimensions;
    float difference_scale = 1.0F;
    double mean_absolute_difference = 0.0;
    double maximum_absolute_difference = 0.0;
};

[[nodiscard]] ReferenceRenderOutputs render_darktable_reference_comparison(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::filesystem::path& output_directory
) {
    constexpr std::uint32_t maximum_edge = 1'536U;
    const auto reference = make_darktable_opposed_reference_plane(frame, transform);
    if (!reference.valid()) {
        throw std::runtime_error("darktable opposed reference produced an invalid CFA plane");
    }
    const Dimensions dimensions = reference_preview_dimensions(reference.dimensions, maximum_edge);
    const auto shadow_grid = detail::make_bayer_area_sampling_grid(frame, dimensions);
    const auto shadow_policy = detail::editable_raw_cfa_sampling_policy(transform);
    const auto sample_count = static_cast<std::size_t>(dimensions.pixel_count()) * 3U;
    std::vector<float> shadow_scene(sample_count);
    std::vector<float> darktable_scene(sample_count);
    std::vector<float> differences(sample_count);
    std::vector<float> per_pixel_maximum(static_cast<std::size_t>(dimensions.pixel_count()));
    long double absolute_difference = 0.0L;
    double maximum_difference = 0.0;
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const auto shadow_camera = detail::area_camera_rgb_sample_at(
                                           frame,
                                           shadow_grid,
                                           x,
                                           y,
                                           &transform,
                                           shadow_policy
            )
                                           .values;
            const auto darktable_camera =
                darktable_opposed_area_camera_rgb_at(reference, dimensions, x, y);
            const auto shadow_rgb = camera_rgb_to_scene_rgb(transform, shadow_camera);
            const auto darktable_rgb = camera_rgb_to_scene_rgb(transform, darktable_camera);
            const auto pixel = static_cast<std::size_t>(y) * dimensions.width + x;
            float pixel_maximum = 0.0F;
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                const auto index = pixel * 3U + channel;
                shadow_scene[index] = shadow_rgb[channel];
                darktable_scene[index] = darktable_rgb[channel];
                differences[index] = std::abs(shadow_rgb[channel] - darktable_rgb[channel]);
                pixel_maximum = std::max(pixel_maximum, differences[index]);
                absolute_difference += differences[index];
                maximum_difference =
                    std::max(maximum_difference, static_cast<double>(differences[index]));
            }
            per_pixel_maximum[pixel] = pixel_maximum;
        }
    }
    const std::size_t percentile_index =
        per_pixel_maximum.empty() ? 0U : (per_pixel_maximum.size() - 1U) * 995U / 1'000U;
    if (!per_pixel_maximum.empty()) {
        std::nth_element(
            per_pixel_maximum.begin(),
            per_pixel_maximum.begin() + static_cast<std::ptrdiff_t>(percentile_index),
            per_pixel_maximum.end()
        );
    }
    const float percentile = per_pixel_maximum.empty() ? 0.0F : per_pixel_maximum[percentile_index];
    const float difference_scale = percentile <= 1.0e-8F ? 1.0F : 0.5F / percentile;

    ReferenceRenderOutputs outputs{
        .shadow_pfm = output_directory / "highlight-reference-shadow-current-linear.pfm",
        .darktable_pfm = output_directory / "highlight-reference-darktable-opposed-linear.pfm",
        .shadow_ppm = output_directory / "highlight-reference-shadow-current-display.ppm",
        .darktable_ppm = output_directory / "highlight-reference-darktable-opposed-display.ppm",
        .difference_ppm = output_directory / "highlight-reference-absolute-difference.ppm",
        .dimensions = dimensions,
        .difference_scale = difference_scale,
        .mean_absolute_difference =
            sample_count == 0U
                ? 0.0
                : static_cast<double>(absolute_difference / static_cast<long double>(sample_count)),
        .maximum_absolute_difference = maximum_difference,
    };
    write_float_pfm(outputs.shadow_pfm, dimensions, shadow_scene);
    write_float_pfm(outputs.darktable_pfm, dimensions, darktable_scene);
    write_display_ppm(outputs.shadow_ppm, dimensions, shadow_scene);
    write_display_ppm(outputs.darktable_ppm, dimensions, darktable_scene);
    write_display_ppm(outputs.difference_ppm, dimensions, differences, difference_scale);

    constexpr std::array<const char*, 3U> channel_names{"R", "G", "B"};
    std::cout << "highlight_reference.algorithm=darktable-opposed-source-943d74a50e5b\n"
              << "highlight_reference.stage=post-rawframe-pre-demosaic\n"
              << "highlight_reference.shared_downstream=shadow-area-sampling-and-camera-matrix\n"
              << "highlight_reference.dimensions=" << dimensions.width << 'x' << dimensions.height
              << '\n'
              << "highlight_reference.clipped_photosites=" << reference.clipped_photosites << '\n'
              << "highlight_reference.changed_photosites=" << reference.changed_photosites << '\n';
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        std::cout << "highlight_reference.channel." << channel_names[channel]
                  << ".clip=" << reference.clip_values[channel] << '\n'
                  << "highlight_reference.channel." << channel_names[channel]
                  << ".chrominance_offset=" << reference.chrominance_offsets[channel] << '\n'
                  << "highlight_reference.channel." << channel_names[channel]
                  << ".chrominance_support=" << reference.chrominance_support[channel] << '\n';
    }
    return outputs;
}

[[nodiscard]] ProfileDerivativeStatistics write_horizontal_profile(
    const std::filesystem::path& path,
    const std::vector<ProfileRow>& profile,
    const DomainDeltaSite& center
) {
    std::ofstream csv(path);
    if (!csv) {
        throw std::runtime_error("cannot create " + path.string());
    }
    csv << "x,y,cfa,terminal,common_measured,common_reconstructed,darktable_measured,"
           "darktable_reconstructed,common_scene_r,common_scene_g,common_scene_b,"
           "darktable_scene_r,darktable_scene_g,darktable_scene_b,common_luminance,"
           "darktable_luminance,common_d1,darktable_d1,common_d2,darktable_d2\n";
    constexpr std::array<const char*, 3U> channel_names{"R", "G", "B"};
    constexpr std::uint32_t statistics_radius = 64U;
    ProfileDerivativeStatistics statistics;
    for (std::size_t index = 0U; index < profile.size(); ++index) {
        const auto& row = profile[index];
        const double common_d1 =
            index == 0U ? 0.0
                        : row.common_scene_luminance - profile[index - 1U].common_scene_luminance;
        const double darktable_d1 =
            index == 0U
                ? 0.0
                : row.darktable_scene_luminance - profile[index - 1U].darktable_scene_luminance;
        const double common_d2 = index == 0U || index + 1U >= profile.size()
                                     ? 0.0
                                     : profile[index + 1U].common_scene_luminance
                                           - 2.0 * row.common_scene_luminance
                                           + profile[index - 1U].common_scene_luminance;
        const double darktable_d2 = index == 0U || index + 1U >= profile.size()
                                        ? 0.0
                                        : profile[index + 1U].darktable_scene_luminance
                                              - 2.0 * row.darktable_scene_luminance
                                              + profile[index - 1U].darktable_scene_luminance;
        const std::uint32_t distance = row.x > center.x ? row.x - center.x : center.x - row.x;
        if (distance <= statistics_radius) {
            statistics.common_first_maximum =
                std::max(statistics.common_first_maximum, std::abs(common_d1));
            statistics.darktable_first_maximum =
                std::max(statistics.darktable_first_maximum, std::abs(darktable_d1));
            statistics.common_second_maximum =
                std::max(statistics.common_second_maximum, std::abs(common_d2));
            statistics.darktable_second_maximum =
                std::max(statistics.darktable_second_maximum, std::abs(darktable_d2));
        }
        csv << row.x << ',' << center.y << ',' << channel_names[row.channel] << ','
            << (row.common_white.terminal_candidate ? 1 : 0) << ',' << row.common_white.measured
            << ',' << row.common_white.reconstructed << ',' << row.darktable_domain.measured << ','
            << row.darktable_domain.reconstructed << ',' << row.common_scene_rgb[0] << ','
            << row.common_scene_rgb[1] << ',' << row.common_scene_rgb[2] << ','
            << row.darktable_scene_rgb[0] << ',' << row.darktable_scene_rgb[1] << ','
            << row.darktable_scene_rgb[2] << ',' << row.common_scene_luminance << ','
            << row.darktable_scene_luminance << ',' << common_d1 << ',' << darktable_d1 << ','
            << common_d2 << ',' << darktable_d2 << '\n';
    }
    if (!csv) {
        throw std::runtime_error("cannot write " + path.string());
    }
    return statistics;
}

} // namespace

void render_highlight_cfa_domain_diagnostic(
    const RawFrame& frame,
    const RawFrameLinearTransform& transform,
    const std::filesystem::path& output_directory
) {
    const auto reference_outputs =
        render_darktable_reference_comparison(frame, transform, output_directory);
    auto common_treatment = detail::editable_raw_cfa_sampling_policy(transform);
    common_treatment.preserve_terminal_white_balance_headroom = false;
    auto darktable_treatment = detail::editable_raw_cfa_sampling_policy(transform);
    darktable_treatment.require_shared_terminal_headroom = false;
    const auto darktable_chrominance = detail::estimate_opposed_highlight_chrominance_correction(
        frame,
        &transform,
        darktable_treatment,
        4U
    );

    const auto& descriptor = frame.descriptor;
    const std::uint32_t first_x = descriptor.active_margins.left;
    const std::uint32_t first_y = descriptor.active_margins.top;
    const std::uint32_t last_x = first_x + descriptor.active_dimensions.width;
    const std::uint32_t last_y = first_y + descriptor.active_dimensions.height;
    std::vector<DomainDeltaSite> largest;
    largest.reserve(512U);
    std::optional<DomainDeltaSite> strongest_boundary;
    float strongest_boundary_difference = 0.0F;

    for (std::uint32_t y = first_y; y < last_y; ++y) {
        std::optional<DomainDeltaSite> previous;
        for (std::uint32_t x = first_x; x < last_x; ++x) {
            const auto channel = cfa_rgb_index(descriptor.bayer_2x2[(y & 1U) * 2U + (x & 1U)]);
            if (!channel.has_value()) {
                continue;
            }
            const DomainDeltaSite site{
                .x = x,
                .y = y,
                .channel = *channel,
                .common_white = detail::opposed_highlight_cfa_sample_at(
                    frame,
                    x,
                    y,
                    &transform,
                    common_treatment
                ),
                .darktable_domain = detail::opposed_highlight_cfa_sample_at(
                    frame,
                    x,
                    y,
                    &transform,
                    darktable_treatment
                ),
            };
            if (previous.has_value()
                && previous->common_white.terminal_candidate
                       != site.common_white.terminal_candidate) {
                const auto& terminal = site.common_white.terminal_candidate ? site : *previous;
                const float difference = domain_difference(terminal);
                if (difference > strongest_boundary_difference) {
                    strongest_boundary_difference = difference;
                    strongest_boundary = terminal;
                }
            }
            previous = site;
            if (site.common_white.terminal_candidate && domain_difference(site) > 1.0e-6F) {
                largest.push_back(site);
                if (largest.size() >= 512U) {
                    retain_largest_domain_differences(largest);
                }
            }
        }
    }
    retain_largest_domain_differences(largest);
    std::sort(
        largest.begin(),
        largest.end(),
        [](const DomainDeltaSite& left, const DomainDeltaSite& right) {
            return domain_difference(left) > domain_difference(right);
        }
    );
    if (largest.size() > 64U) {
        largest.resize(64U);
    }

    const std::filesystem::path comparison_path =
        output_directory / "highlight-cfa-domain-comparison.csv";
    std::ofstream comparison(comparison_path);
    if (!comparison) {
        throw std::runtime_error("cannot create " + comparison_path.string());
    }
    comparison
        << "x,y,cfa,common_measured,common_reference,common_reconstructed,"
           "darktable_measured,darktable_reference,darktable_reconstructed,domain_delta,"
           "common_scene_r,common_scene_g,common_scene_b,common_scene_luminance,"
           "darktable_scene_r,darktable_scene_g,darktable_scene_b,darktable_scene_luminance\n";
    constexpr std::array<const char*, 3U> channel_names{"R", "G", "B"};
    for (const auto& site : largest) {
        const auto common_rgb = detail::bilinear_camera_rgb_sample_at(
            frame,
            site.x,
            site.y,
            &transform,
            common_treatment
        );
        const auto darktable_rgb = detail::bilinear_camera_rgb_sample_at(
            frame,
            site.x,
            site.y,
            &transform,
            darktable_treatment
        );
        const auto common_scene = camera_sample_to_scene_rgb(transform, common_rgb);
        const auto darktable_scene = camera_sample_to_scene_rgb(transform, darktable_rgb);
        comparison << site.x << ',' << site.y << ',' << channel_names[site.channel] << ','
                   << site.common_white.measured << ',' << site.common_white.opposed_reference
                   << ',' << site.common_white.reconstructed << ','
                   << site.darktable_domain.measured << ','
                   << site.darktable_domain.opposed_reference << ','
                   << site.darktable_domain.reconstructed << ',' << domain_difference(site) << ','
                   << common_scene[0] << ',' << common_scene[1] << ',' << common_scene[2] << ','
                   << linear_srgb_luminance(common_scene) << ',' << darktable_scene[0] << ','
                   << darktable_scene[1] << ',' << darktable_scene[2] << ','
                   << linear_srgb_luminance(darktable_scene) << '\n';
    }
    if (!comparison) {
        throw std::runtime_error("cannot write " + comparison_path.string());
    }

    const std::optional<DomainDeltaSite> profile_center =
        strongest_boundary.has_value()
            ? strongest_boundary
            : (largest.empty() ? std::optional<DomainDeltaSite>{}
                               : std::optional<DomainDeltaSite>{largest.front()});
    const std::filesystem::path profile_path =
        output_directory / "highlight-cfa-domain-horizontal-profile.csv";
    ProfileDerivativeStatistics derivative_statistics;
    if (profile_center.has_value()) {
        derivative_statistics = write_horizontal_profile(
            profile_path,
            make_horizontal_profile(
                frame,
                transform,
                common_treatment,
                darktable_treatment,
                profile_center->y
            ),
            *profile_center
        );
    }

    std::cout << "highlight_cfa_diagnostic.domain.darktable_headroom=yes\n"
              << "highlight_cfa_diagnostic.domain.darktable_chrominance_applied=yes\n";
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        std::cout << "highlight_cfa_diagnostic.domain.channel." << channel_names[channel]
                  << ".sampled_chrominance_offset=" << darktable_chrominance.offsets[channel]
                  << '\n'
                  << "highlight_cfa_diagnostic.domain.channel." << channel_names[channel]
                  << ".sampled_chrominance_support="
                  << darktable_chrominance.supporting_samples[channel] << '\n';
    }
    std::cout << "highlight_cfa_diagnostic.domain.largest_sites=" << largest.size() << '\n'
              << "highlight_cfa_diagnostic.domain.strongest_boundary_difference="
              << strongest_boundary_difference << '\n'
              << "highlight_cfa_diagnostic.domain.profile_center="
              << (profile_center.has_value()
                      ? std::to_string(profile_center->x) + "," + std::to_string(profile_center->y)
                      : "unavailable")
              << '\n'
              << "highlight_cfa_diagnostic.domain.profile.common_first_maximum="
              << derivative_statistics.common_first_maximum << '\n'
              << "highlight_cfa_diagnostic.domain.profile.darktable_first_maximum="
              << derivative_statistics.darktable_first_maximum << '\n'
              << "highlight_cfa_diagnostic.domain.profile.common_second_maximum="
              << derivative_statistics.common_second_maximum << '\n'
              << "highlight_cfa_diagnostic.domain.profile.darktable_second_maximum="
              << derivative_statistics.darktable_second_maximum << '\n'
              << "highlight_cfa_diagnostic.domain.csv=" << comparison_path.string() << '\n'
              << "highlight_cfa_diagnostic.domain.profile_csv="
              << (profile_center.has_value() ? profile_path.string() : "unavailable") << '\n'
              << "highlight_reference.mean_absolute_difference="
              << reference_outputs.mean_absolute_difference << '\n'
              << "highlight_reference.maximum_absolute_difference="
              << reference_outputs.maximum_absolute_difference << '\n'
              << "highlight_reference.difference_scale=" << reference_outputs.difference_scale
              << '\n'
              << "highlight_reference.shadow_linear=" << reference_outputs.shadow_pfm.string()
              << '\n'
              << "highlight_reference.darktable_linear=" << reference_outputs.darktable_pfm.string()
              << '\n'
              << "highlight_reference.shadow_display=" << reference_outputs.shadow_ppm.string()
              << '\n'
              << "highlight_reference.darktable_display="
              << reference_outputs.darktable_ppm.string() << '\n'
              << "highlight_reference.absolute_difference="
              << reference_outputs.difference_ppm.string() << '\n';
}

} // namespace shadow::image::probe_detail

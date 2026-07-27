#include <shadow/image/neutral_balance.hpp>
#include <shadow/image/working_rgb.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace shadow::image {

namespace {

inline constexpr std::uint64_t minimum_eligible_pixel_count = 8U;

struct CandidateWork final {
    NeutralBalanceCandidate candidate;
    double log_red_to_green = 0.0;
    double log_blue_to_green = 0.0;
};

[[nodiscard]] double smoothstep(
    const double edge_zero,
    const double edge_one,
    const double value
) noexcept {
    const double t = std::clamp(
        (value - edge_zero) / (edge_one - edge_zero),
        0.0,
        1.0
    );
    return t * t * (3.0 - 2.0 * t);
}

void validate_options(const NeutralBalanceAnalysisOptions& options) {
    if (!std::isfinite(options.minimum_luminance) || options.minimum_luminance <= 0.0
        || !std::isfinite(options.maximum_luminance)
        || options.maximum_luminance <= options.minimum_luminance
        || !std::isfinite(options.maximum_relative_chroma)
        || options.maximum_relative_chroma <= 0.0
        || options.maximum_relative_chroma > 1.0
        || options.maximum_candidate_count == 0U || options.maximum_candidate_count > 32U) {
        throw std::invalid_argument("neutral-balance analysis options are outside their bounds");
    }
}

[[nodiscard]] std::size_t checked_row_floats(const FloatRgbImage& image) {
    if (image.dimensions.width == 0U || image.dimensions.height == 0U
        || image.pixel_format != FloatPixelFormat::rgb_f32_native_interleaved
        || image.transfer_function != TransferFunction::linear
        || image.reference != ImageReference::scene_referred
        || image.row_stride_bytes % sizeof(float) != 0U) {
        throw std::invalid_argument("neutral-balance analysis requires scene-linear RGB input");
    }
    const std::size_t row_floats = image.row_stride_bytes / sizeof(float);
    const std::size_t active_floats = static_cast<std::size_t>(image.dimensions.width) * 3U;
    const std::size_t rows = static_cast<std::size_t>(image.dimensions.height);
    if (row_floats < active_floats || rows > std::numeric_limits<std::size_t>::max() / row_floats
        || image.samples.size() < rows * row_floats) {
        throw std::invalid_argument("neutral-balance analysis received an invalid RGB layout");
    }
    const bool valid_luminance = std::ranges::all_of(
        image.working_space.luminance_coefficients,
        [](const double value) { return std::isfinite(value) && value >= 0.0; }
    );
    const double luminance_sum = image.working_space.luminance_coefficients[0]
        + image.working_space.luminance_coefficients[1]
        + image.working_space.luminance_coefficients[2];
    if (!valid_luminance || luminance_sum <= 0.0) {
        throw std::invalid_argument("neutral-balance analysis requires finite luminance coefficients");
    }
    return row_floats;
}

[[nodiscard]] double weighted_median(std::vector<std::pair<double, double>> values) {
    std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    double total_weight = 0.0;
    for (const auto& [unused_value, weight] : values) {
        static_cast<void>(unused_value);
        total_weight += weight;
    }
    double cumulative = 0.0;
    for (const auto& [value, weight] : values) {
        cumulative += weight;
        if (cumulative * 2.0 >= total_weight) {
            return value;
        }
    }
    return values.empty() ? 0.0 : values.back().first;
}

[[nodiscard]] bool spatially_distinct(
    const NeutralBalanceCandidate& candidate,
    const std::vector<NeutralBalanceCandidate>& selected,
    const double minimum_distance_squared
) noexcept {
    return std::ranges::all_of(selected, [&candidate, minimum_distance_squared](
        const NeutralBalanceCandidate& existing
    ) {
        const double dx = static_cast<double>(candidate.x) - existing.x;
        const double dy = static_cast<double>(candidate.y) - existing.y;
        return dx * dx + dy * dy >= minimum_distance_squared;
    });
}

} // namespace

NeutralBalanceAnalysis analyze_scene_linear_neutral_balance(
    const FloatRgbImage& image,
    const NeutralBalanceAnalysisOptions options
) {
    validate_options(options);
    const std::size_t row_floats = checked_row_floats(image);

    NeutralBalanceAnalysis result;
    std::vector<CandidateWork> eligible;
    const auto maximum_samples = static_cast<std::uint64_t>(image.dimensions.width)
        * image.dimensions.height;
    eligible.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(maximum_samples, 262'144U)));

    for (std::uint32_t y = 0U; y < image.dimensions.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * row_floats;
        for (std::uint32_t x = 0U; x < image.dimensions.width; ++x) {
            const std::size_t index = row + static_cast<std::size_t>(x) * 3U;
            const double red = image.samples[index];
            const double green = image.samples[index + 1U];
            const double blue = image.samples[index + 2U];
            ++result.sampled_pixel_count;
            if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue)
                || red <= 0.0 || green <= 0.0 || blue <= 0.0) {
                continue;
            }
            const double peak = std::max({red, green, blue});
            const double floor = std::min({red, green, blue});
            const double luminance = red * image.working_space.luminance_coefficients[0]
                + green * image.working_space.luminance_coefficients[1]
                + blue * image.working_space.luminance_coefficients[2];
            if (!std::isfinite(luminance) || luminance < options.minimum_luminance
                || luminance > options.maximum_luminance) {
                continue;
            }
            const double relative_chroma = (peak - floor) / peak;
            if (relative_chroma > options.maximum_relative_chroma) {
                continue;
            }
            const double low_luminance_weight = smoothstep(
                options.minimum_luminance,
                options.minimum_luminance * 2.0,
                luminance
            );
            const double high_luminance_weight = 1.0 - smoothstep(
                options.maximum_luminance * 0.85,
                options.maximum_luminance,
                luminance
            );
            const double confidence = low_luminance_weight * high_luminance_weight
                * (1.0 - smoothstep(
                    options.maximum_relative_chroma * 0.5,
                    options.maximum_relative_chroma,
                    relative_chroma
                ));
            if (confidence <= 0.0) {
                continue;
            }
            eligible.push_back(CandidateWork{
                .candidate = {
                    .x = x,
                    .y = y,
                    .red = red,
                    .green = green,
                    .blue = blue,
                    .luminance = luminance,
                    .relative_chroma = relative_chroma,
                    .confidence = confidence,
                },
                .log_red_to_green = std::log(red / green),
                .log_blue_to_green = std::log(blue / green),
            });
        }
    }

    result.eligible_pixel_count = eligible.size();
    if (result.eligible_pixel_count < minimum_eligible_pixel_count) {
        return result;
    }

    std::vector<std::pair<double, double>> red_ratios;
    std::vector<std::pair<double, double>> blue_ratios;
    red_ratios.reserve(eligible.size());
    blue_ratios.reserve(eligible.size());
    double total_confidence = 0.0;
    for (const CandidateWork& entry : eligible) {
        red_ratios.emplace_back(entry.log_red_to_green, entry.candidate.confidence);
        blue_ratios.emplace_back(entry.log_blue_to_green, entry.candidate.confidence);
        total_confidence += entry.candidate.confidence;
    }
    result.red_to_green = std::exp(weighted_median(std::move(red_ratios)));
    result.blue_to_green = std::exp(weighted_median(std::move(blue_ratios)));
    result.suggested_red_gain = 1.0 / result.red_to_green;
    result.suggested_blue_gain = 1.0 / result.blue_to_green;

    std::sort(eligible.begin(), eligible.end(), [](const CandidateWork& left, const CandidateWork& right) {
        return left.candidate.confidence > right.candidate.confidence;
    });
    const double shorter_side = std::min(image.dimensions.width, image.dimensions.height);
    const double minimum_distance_squared = std::max(1.0, shorter_side * shorter_side / 100.0);
    for (const CandidateWork& entry : eligible) {
        if (result.candidates.size() >= options.maximum_candidate_count) {
            break;
        }
        if (spatially_distinct(entry.candidate, result.candidates, minimum_distance_squared)) {
            result.candidates.push_back(entry.candidate);
        }
    }
    if (result.candidates.empty()) {
        return {};
    }
    const double mean_candidate_confidence = total_confidence
        / static_cast<double>(result.eligible_pixel_count);
    const double evidence = smoothstep(
        static_cast<double>(minimum_eligible_pixel_count),
        64.0,
        static_cast<double>(result.eligible_pixel_count)
    );
    const double coverage = std::min(
        1.0,
        static_cast<double>(result.candidates.size()) / 3.0
    );
    result.confidence = mean_candidate_confidence * evidence * coverage;
    result.available = std::isfinite(result.red_to_green)
        && std::isfinite(result.blue_to_green)
        && std::isfinite(result.suggested_red_gain)
        && std::isfinite(result.suggested_blue_gain)
        && result.confidence > 0.0;
    return result;
}

} // namespace shadow::image

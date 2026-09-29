#include "../src/raw/clipped_highlight_reconstruction.hpp"

#include <shadow/image/raw_development.hpp>
#include <shadow/image/sensor_clipping.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace image = shadow::image;

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] float luminance(const image::SceneLinearRgbFrame& frame, const std::size_t pixel) {
    const auto sample = pixel * 3U;
    return frame.samples[sample] * 0.2126F + frame.samples[sample + 1U] * 0.7152F
           + frame.samples[sample + 2U] * 0.0722F;
}

[[nodiscard]] image::SceneLinearRgbFrame smooth_surface(const image::Dimensions dimensions) {
    image::SceneLinearRgbFrame frame{
        .dimensions = dimensions,
        .row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float),
        .samples = std::vector<float>(static_cast<std::size_t>(dimensions.pixel_count()) * 3U),
    };
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const auto sample = (static_cast<std::size_t>(y) * dimensions.width + x) * 3U;
            frame.samples[sample] =
                1.15F + static_cast<float>(x) * 0.0040F + static_cast<float>(y) * 0.0015F;
            frame.samples[sample + 1U] =
                1.05F + static_cast<float>(x) * 0.0035F + static_cast<float>(y) * 0.0018F;
            frame.samples[sample + 2U] =
                0.88F + static_cast<float>(x) * 0.0030F + static_cast<float>(y) * 0.0012F;
        }
    }
    return frame;
}

[[nodiscard]] image::SensorClippingMask circular_clipping_mask(
    const image::Dimensions dimensions,
    const std::uint32_t centre_x,
    const std::uint32_t centre_y,
    const std::uint32_t radius
) {
    image::SensorClippingMask mask{
        .dimensions = dimensions,
        .samples = std::vector<std::uint8_t>(static_cast<std::size_t>(dimensions.pixel_count())),
    };
    const auto radius_squared = static_cast<std::int64_t>(radius) * radius;
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const auto dx = static_cast<std::int64_t>(x) - centre_x;
            const auto dy = static_cast<std::int64_t>(y) - centre_y;
            if (dx * dx + dy * dy <= radius_squared) {
                const auto pixel = static_cast<std::size_t>(y) * dimensions.width + x;
                mask.samples[pixel] = static_cast<std::uint8_t>(
                    image::sensor_highlight_clipped | image::sensor_shared_highlight_clipped
                );
                ++mask.highlight_pixel_count;
            }
        }
    }
    return mask;
}

[[nodiscard]] image::HighlightChromaRiskMap
highlight_risk_from_mask(const image::SensorClippingMask& mask) {
    image::HighlightChromaRiskMap risk{
        .dimensions = mask.dimensions,
        .samples = std::vector<std::uint8_t>(mask.samples.size()),
    };
    for (std::size_t pixel = 0U; pixel < mask.samples.size(); ++pixel) {
        risk.samples[pixel] =
            (mask.samples[pixel] & image::sensor_highlight_clipped) != 0U ? 255U : 0U;
    }
    return risk;
}

void cfa_completion_retains_only_bounded_terminal_uncertainty() {
    image::SensorClippingMask clipping{
        .dimensions = {3U, 1U},
        .samples = {
            static_cast<std::uint8_t>(
                image::sensor_highlight_clipped | image::sensor_shared_highlight_clipped
            ),
            static_cast<std::uint8_t>(
                image::sensor_highlight_clipped
                | (16U << image::sensor_shared_highlight_coverage_shift)
            ),
            image::sensor_highlight_clipped,
        },
        .highlight_pixel_count = 3U,
    };
    image::HighlightChromaRiskMap risk{
        .dimensions = clipping.dimensions,
        .samples = {255U, 255U, 255U},
    };

    image::raw_pipeline_detail::complete_cfa_owned_highlight_reconstruction(clipping, risk);

    expect(
        risk.source_surface_reconstructed && risk.samples[0U] == 64U,
        "a fully terminal reconstructed core retains only bounded residual colour uncertainty"
    );
    expect(
        risk.samples[1U] > 0U && risk.samples[1U] < risk.samples[0U],
        "fractional shared-terminal ownership fades residual uncertainty continuously"
    );
    expect(
        risk.samples[2U] == 0U,
        "an any-channel clipping boundary cannot inherit reconstructed terminal uncertainty"
    );
}

[[nodiscard]] double clipped_chroma_rmse(
    const image::SceneLinearRgbFrame& actual,
    const image::SceneLinearRgbFrame& expected,
    const image::SensorClippingMask& clipping
) {
    double squared_error = 0.0;
    std::uint64_t count = 0U;
    for (std::size_t pixel = 0U; pixel < clipping.samples.size(); ++pixel) {
        if ((clipping.samples[pixel] & image::sensor_highlight_clipped) == 0U) {
            continue;
        }
        const auto sample = pixel * 3U;
        const auto chroma = [&](const image::SceneLinearRgbFrame& frame,
                                const std::size_t channel) {
            const float sum = std::max(
                frame.samples[sample] + frame.samples[sample + 1U] + frame.samples[sample + 2U],
                1.0e-6F
            );
            return frame.samples[sample + channel] / sum;
        };
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            const double difference = static_cast<double>(chroma(actual, channel))
                                      - static_cast<double>(chroma(expected, channel));
            squared_error += difference * difference;
            ++count;
        }
    }
    return std::sqrt(squared_error / static_cast<double>(count));
}

[[nodiscard]] double maximum_luminance_change(
    const image::SceneLinearRgbFrame& actual,
    const image::SceneLinearRgbFrame& expected
) {
    double maximum = 0.0;
    for (std::size_t pixel = 0U; pixel < actual.dimensions.pixel_count(); ++pixel) {
        maximum = std::max(
            maximum,
            std::abs(
                static_cast<double>(luminance(actual, pixel))
                - static_cast<double>(luminance(expected, pixel))
            )
        );
    }
    return maximum;
}

[[nodiscard]] double maximum_unclipped_luminance_change(
    const image::SceneLinearRgbFrame& actual,
    const image::SceneLinearRgbFrame& expected,
    const image::SensorClippingMask& clipping
) {
    double maximum = 0.0;
    for (std::size_t pixel = 0U; pixel < clipping.samples.size(); ++pixel) {
        if ((clipping.samples[pixel] & image::sensor_highlight_clipped) != 0U) {
            continue;
        }
        maximum = std::max(
            maximum,
            std::abs(
                static_cast<double>(luminance(actual, pixel))
                - static_cast<double>(luminance(expected, pixel))
            )
        );
    }
    return maximum;
}

[[nodiscard]] double maximum_clipping_boundary_step(
    const image::SceneLinearRgbFrame& frame,
    const image::SensorClippingMask& clipping
) {
    double maximum = 0.0;
    const auto width = clipping.dimensions.width;
    const auto height = clipping.dimensions.height;
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * width + x;
            const bool clipped = (clipping.samples[pixel] & image::sensor_highlight_clipped) != 0U;
            const auto compare = [&](const std::uint32_t neighbour_x,
                                     const std::uint32_t neighbour_y) {
                const auto neighbour = static_cast<std::size_t>(neighbour_y) * width + neighbour_x;
                const bool neighbour_clipped =
                    (clipping.samples[neighbour] & image::sensor_highlight_clipped) != 0U;
                if (clipped != neighbour_clipped) {
                    const double difference = std::abs(
                        static_cast<double>(luminance(frame, pixel))
                        - static_cast<double>(luminance(frame, neighbour))
                    );
                    if (difference > maximum) {
                        maximum = difference;
                    }
                }
            };
            if (x + 1U < width) {
                compare(x + 1U, y);
            }
            if (y + 1U < height) {
                compare(x, y + 1U);
            }
        }
    }
    return maximum;
}

void reconstruction_hides_clip_topology_without_crossing_dark_edges() {
    constexpr image::Dimensions dimensions{192U, 128U};
    constexpr std::uint32_t centre_x = 102U;
    constexpr std::uint32_t centre_y = 60U;
    constexpr std::uint32_t radius = 27U;
    const auto expected = smooth_surface(dimensions);
    auto clipping = circular_clipping_mask(dimensions, centre_x, centre_y, radius);
    auto damaged = expected;
    for (std::size_t pixel = 0U; pixel < clipping.samples.size(); ++pixel) {
        if ((clipping.samples[pixel] & image::sensor_highlight_clipped) == 0U) {
            continue;
        }
        const auto sample = pixel * 3U;
        damaged.samples[sample] = 3.8F;
        damaged.samples[sample + 1U] = 0.42F;
        damaged.samples[sample + 2U] = 2.9F;
    }

    // A measured dark subject touches the highlight shoulder but is not itself clipped. The
    // effect-priority reconstruction may lift or recolour bright measured neighbours; it must not
    // paint the reconstructed highlight across this real edge.
    for (std::uint32_t y = 42U; y <= 78U; ++y) {
        for (std::uint32_t x = 67U; x <= 73U; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * dimensions.width + x;
            const auto sample = pixel * 3U;
            damaged.samples[sample] = 0.018F;
            damaged.samples[sample + 1U] = 0.014F;
            damaged.samples[sample + 2U] = 0.010F;
        }
    }
    const auto before = damaged;
    auto highlight_risk = highlight_risk_from_mask(clipping);
    const double before_chroma_error = clipped_chroma_rmse(before, expected, clipping);
    const double before_step = maximum_clipping_boundary_step(before, clipping);
    const auto stats = image::raw_pipeline_detail::reconstruct_clipped_highlight_surface(
        damaged,
        clipping,
        highlight_risk
    );
    const double after_chroma_error = clipped_chroma_rmse(damaged, expected, clipping);
    const double after_step = maximum_clipping_boundary_step(damaged, clipping);
    const double luminance_change = maximum_luminance_change(damaged, before);
    const double unclipped_luminance_change =
        maximum_unclipped_luminance_change(damaged, before, clipping);
    if (!(after_chroma_error < before_chroma_error * 0.30) || !(after_step <= before_step * 1.01)) {
        std::cerr << "diagnostic: clipped chroma RMSE " << before_chroma_error << " -> "
                  << after_chroma_error << ", luminance change " << luminance_change
                  << ", boundary step " << before_step << " -> " << after_step << '\n';
    }

    expect(
        stats.clipped_pixel_count == clipping.highlight_pixel_count
            && stats.blended_pixel_count > 0U
            && stats.blended_pixel_count <= stats.clipped_pixel_count,
        "source reconstruction remains inside the factual clipped footprint"
    );
    expect(
        stats.guide_dimensions == dimensions,
        "small sources reconstruct directly at their source dimensions"
    );
    expect(
        after_chroma_error < before_chroma_error * 0.30,
        "low-frequency reconstruction replaces false clipped colour with the measured trend"
    );
    expect(
        after_step <= before_step * 1.01,
        "chroma reconstruction does not enlarge the factual clipping luminance step"
    );
    expect(
        unclipped_luminance_change < 2.0e-5,
        "bounded exterior chroma repair preserves exact measured luminance outside clipping"
    );
    const auto centre_pixel = static_cast<std::size_t>(centre_y) * dimensions.width + centre_x;
    expect(
        luminance(damaged, centre_pixel) > luminance(before, centre_pixel) * 0.82F,
        "contradictory-core smoothing retains most clipped-source energy instead of flattening it"
    );
    expect(
        highlight_risk.source_surface_reconstructed && highlight_risk.samples[centre_pixel] == 64U,
        "the grade sidecar keeps only bounded terminal uncertainty after source reconstruction"
    );

    const auto dark_pixel = static_cast<std::size_t>(60U) * dimensions.width + 70U;
    const auto dark_sample = dark_pixel * 3U;
    expect(
        std::abs(damaged.samples[dark_sample] - before.samples[dark_sample]) < 1.0e-5F
            && std::abs(damaged.samples[dark_sample + 1U] - before.samples[dark_sample + 1U])
                   < 1.0e-5F
            && std::abs(damaged.samples[dark_sample + 2U] - before.samples[dark_sample + 2U])
                   < 1.0e-5F,
        "the brightness gate does not bleed reconstructed highlights across a dark subject"
    );
    const auto far_pixel = static_cast<std::size_t>(8U) * dimensions.width + 8U;
    const auto far_sample = far_pixel * 3U;
    expect(
        damaged.samples[far_sample] == before.samples[far_sample]
            && damaged.samples[far_sample + 1U] == before.samples[far_sample + 1U]
            && damaged.samples[far_sample + 2U] == before.samples[far_sample + 2U],
        "source-trustworthy pixels outside the bounded shoulder remain byte-identical"
    );
}

void measured_highlight_shoulder_remains_exact() {
    constexpr image::Dimensions dimensions{128U, 96U};
    constexpr std::uint32_t centre_x = 64U;
    constexpr std::uint32_t centre_y = 48U;
    constexpr std::uint32_t clipped_radius = 18U;
    constexpr std::uint32_t shoulder_radius = 27U;
    auto source = image::SceneLinearRgbFrame{
        .dimensions = dimensions,
        .row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float),
        .samples = std::vector<float>(static_cast<std::size_t>(dimensions.pixel_count()) * 3U),
    };
    auto clipping = circular_clipping_mask(dimensions, centre_x, centre_y, clipped_radius);
    const auto clipped_radius_squared = static_cast<std::int64_t>(clipped_radius) * clipped_radius;
    const auto shoulder_radius_squared =
        static_cast<std::int64_t>(shoulder_radius) * shoulder_radius;
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * dimensions.width + x;
            const auto sample = pixel * 3U;
            const auto dx = static_cast<std::int64_t>(x) - centre_x;
            const auto dy = static_cast<std::int64_t>(y) - centre_y;
            const auto distance_squared = dx * dx + dy * dy;
            if (distance_squared <= clipped_radius_squared) {
                source.samples[sample] = 3.2F;
                source.samples[sample + 1U] = 0.45F;
                source.samples[sample + 2U] = 2.6F;
            } else if (distance_squared <= shoulder_radius_squared) {
                source.samples[sample] = 1.45F;
                source.samples[sample + 1U] = 1.18F;
                source.samples[sample + 2U] = 0.78F;
            } else {
                source.samples[sample] = 0.84F;
                source.samples[sample + 1U] = 0.68F;
                source.samples[sample + 2U] = 0.44F;
            }
        }
    }
    const auto before = source;
    auto risk = highlight_risk_from_mask(clipping);
    const auto stats =
        image::raw_pipeline_detail::reconstruct_clipped_highlight_surface(source, clipping, risk);

    bool shoulder_never_darkened = true;
    bool shoulder_was_blended = false;
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const auto dx = static_cast<std::int64_t>(x) - centre_x;
            const auto dy = static_cast<std::int64_t>(y) - centre_y;
            const auto distance_squared = dx * dx + dy * dy;
            if (distance_squared <= clipped_radius_squared
                || distance_squared > shoulder_radius_squared) {
                continue;
            }
            const auto pixel = static_cast<std::size_t>(y) * dimensions.width + x;
            const float before_luminance = luminance(before, pixel);
            const float after_luminance = luminance(source, pixel);
            shoulder_never_darkened =
                shoulder_never_darkened && after_luminance + 1.0e-5F >= before_luminance;
            const auto sample = pixel * 3U;
            shoulder_was_blended =
                shoulder_was_blended
                || std::abs(source.samples[sample] - before.samples[sample]) > 1.0e-4F
                || std::abs(source.samples[sample + 1U] - before.samples[sample + 1U]) > 1.0e-4F
                || std::abs(source.samples[sample + 2U] - before.samples[sample + 2U]) > 1.0e-4F;
        }
    }
    expect(
        stats.blended_pixel_count <= stats.clipped_pixel_count && !shoulder_was_blended,
        "source reconstruction does not paint the first measured highlight shoulder"
    );
    expect(
        shoulder_never_darkened,
        "surface reconstruction never creates a measured-luminance dip for recovery to reveal"
    );
}

void cfa_risk_bands_do_not_modulate_the_luminance_feather() {
    constexpr image::Dimensions dimensions{128U, 96U};
    constexpr std::uint32_t centre_x = 64U;
    constexpr std::uint32_t centre_y = 48U;
    constexpr std::uint32_t clipped_radius = 16U;
    auto source = image::SceneLinearRgbFrame{
        .dimensions = dimensions,
        .row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float),
        .samples = std::vector<float>(static_cast<std::size_t>(dimensions.pixel_count()) * 3U),
    };
    auto clipping = circular_clipping_mask(dimensions, centre_x, centre_y, clipped_radius);
    for (std::size_t pixel = 0U; pixel < dimensions.pixel_count(); ++pixel) {
        const auto sample = pixel * 3U;
        const bool clipped = (clipping.samples[pixel] & image::sensor_highlight_clipped) != 0U;
        source.samples[sample] = clipped ? 1.45F : 0.92F;
        source.samples[sample + 1U] = clipped ? 1.15F : 0.70F;
        source.samples[sample + 2U] = clipped ? 0.72F : 0.42F;
    }
    auto unbanded = source;
    auto banded = source;
    auto unbanded_risk = highlight_risk_from_mask(clipping);
    auto banded_risk = unbanded_risk;
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x < dimensions.width; ++x) {
            const auto dx = static_cast<std::int64_t>(x) - centre_x;
            const auto dy = static_cast<std::int64_t>(y) - centre_y;
            const auto distance_squared = dx * dx + dy * dy;
            const auto pixel = static_cast<std::size_t>(y) * dimensions.width + x;
            if (distance_squared > 17 * 17 && distance_squared <= 23 * 23) {
                banded_risk.samples[pixel] = 224U;
            } else if (distance_squared > 23 * 23 && distance_squared <= 30 * 30) {
                banded_risk.samples[pixel] = 96U;
            }
        }
    }
    static_cast<void>(image::raw_pipeline_detail::reconstruct_clipped_highlight_surface(
        unbanded,
        clipping,
        unbanded_risk
    ));
    static_cast<void>(image::raw_pipeline_detail::reconstruct_clipped_highlight_surface(
        banded,
        clipping,
        banded_risk
    ));

    float maximum_luminance_difference = 0.0F;
    bool residual_risk_outside_shared_terminal = false;
    std::uint8_t maximum_residual_risk = 0U;
    for (std::size_t pixel = 0U; pixel < dimensions.pixel_count(); ++pixel) {
        maximum_luminance_difference = std::max(
            maximum_luminance_difference,
            std::abs(luminance(unbanded, pixel) - luminance(banded, pixel))
        );
        maximum_residual_risk = std::max(maximum_residual_risk, banded_risk.samples[pixel]);
        residual_risk_outside_shared_terminal =
            residual_risk_outside_shared_terminal
            || (banded_risk.samples[pixel] != 0U
                && clipping.shared_highlight_coverage_at(pixel) <= 0.0F);
    }
    if (maximum_luminance_difference >= 2.0e-5F) {
        std::cerr << "diagnostic: risk-band luminance difference " << maximum_luminance_difference
                  << '\n';
    }
    expect(
        maximum_luminance_difference < 2.0e-5F,
        "quantised CFA colour-risk bands cannot become luminance rings around a clipped source"
    );
    expect(
        banded_risk.source_surface_reconstructed && maximum_residual_risk == 64U
            && !residual_risk_outside_shared_terminal,
        "source preparation removes risk bands while retaining only bounded terminal uncertainty"
    );
}

void reconstruction_preserves_bright_warm_highlight_character() {
    constexpr image::Dimensions dimensions{96U, 64U};
    constexpr std::uint32_t centre_x = 48U;
    constexpr std::uint32_t centre_y = 32U;
    auto source = image::SceneLinearRgbFrame{
        .dimensions = dimensions,
        .row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float),
        .samples = std::vector<float>(static_cast<std::size_t>(dimensions.pixel_count()) * 3U),
    };
    for (std::size_t pixel = 0U; pixel < dimensions.pixel_count(); ++pixel) {
        const auto sample = pixel * 3U;
        source.samples[sample] = 0.82F;
        source.samples[sample + 1U] = 0.65F;
        source.samples[sample + 2U] = 0.40F;
    }
    auto clipping = circular_clipping_mask(dimensions, centre_x, centre_y, 15U);
    for (std::size_t pixel = 0U; pixel < clipping.samples.size(); ++pixel) {
        if ((clipping.samples[pixel] & image::sensor_highlight_clipped) == 0U) {
            continue;
        }
        const auto sample = pixel * 3U;
        source.samples[sample] = 1.45F;
        source.samples[sample + 1U] = 1.15F;
        source.samples[sample + 2U] = 0.72F;
    }
    const auto centre_pixel = static_cast<std::size_t>(centre_y) * dimensions.width + centre_x;
    const float before_luminance = luminance(source, centre_pixel);
    auto highlight_risk = highlight_risk_from_mask(clipping);

    static_cast<void>(image::raw_pipeline_detail::reconstruct_clipped_highlight_surface(
        source,
        clipping,
        highlight_risk
    ));

    const auto centre_sample = centre_pixel * 3U;
    const float after_luminance = luminance(source, centre_pixel);
    const float warm_ratio =
        source.samples[centre_sample] / std::max(source.samples[centre_sample + 2U], 1.0e-6F);
    expect(
        after_luminance >= before_luminance * 0.835F,
        "source reconstruction cannot turn a clipped bright core into a low-frequency grey island"
    );
    expect(
        warm_ratio > 1.60F,
        "reconstructed highlight colour follows the measured warm boundary instead of neutral grey"
    );
}

void sparse_projected_cfa_clip_does_not_invent_an_rgb_surface() {
    constexpr image::Dimensions dimensions{96U, 64U};
    constexpr std::uint32_t edge_x = 47U;
    constexpr std::uint32_t edge_y = 32U;
    auto source = smooth_surface(dimensions);
    // Model a dark fixture directly beside a bright lamp. A single projected output pixel can be
    // factually clipped because one contributing CFA sample reached white even though its
    // projected RGB value remains a plausible dark edge sample.
    for (std::uint32_t y = 0U; y < dimensions.height; ++y) {
        for (std::uint32_t x = 0U; x <= edge_x; ++x) {
            const auto sample = (static_cast<std::size_t>(y) * dimensions.width + x) * 3U;
            source.samples[sample] = 0.34F;
            source.samples[sample + 1U] = 0.28F;
            source.samples[sample + 2U] = 0.22F;
        }
    }
    // Put a coherent clipped surface two pixels beyond the fixture edge. Its blurred support
    // deliberately reaches the edge sample, reproducing a target bin whose CFA footprint straddles
    // the lamp and fixture.
    auto clipping = circular_clipping_mask(dimensions, 64U, edge_y, 15U);
    const auto edge_pixel = static_cast<std::size_t>(edge_y) * dimensions.width + edge_x;
    clipping.samples[edge_pixel] = image::sensor_highlight_clipped;
    ++clipping.highlight_pixel_count;
    const auto before = source;
    auto highlight_risk = highlight_risk_from_mask(clipping);

    const auto stats = image::raw_pipeline_detail::reconstruct_clipped_highlight_surface(
        source,
        clipping,
        highlight_risk
    );

    const auto edge_sample = edge_pixel * 3U;
    expect(
        stats.clipped_pixel_count == clipping.highlight_pixel_count,
        "the source receipt remains factual about a projected dark-edge physical clip"
    );
    expect(
        std::abs(source.samples[edge_sample] - before.samples[edge_sample]) < 1.0e-6F
            && std::abs(source.samples[edge_sample + 1U] - before.samples[edge_sample + 1U])
                   < 1.0e-6F
            && std::abs(source.samples[edge_sample + 2U] - before.samples[edge_sample + 2U])
                   < 1.0e-6F,
        "an isolated projected CFA clip cannot grow a bright RGB hair across a mid-dark edge"
    );
    expect(
        highlight_risk.source_surface_reconstructed && highlight_risk.samples[edge_pixel] == 0U,
        "an unreplaced dark-edge clip cannot be neutralised again by the downstream grade"
    );
}

void single_channel_clipping_cannot_become_a_luminance_surface() {
    constexpr image::Dimensions dimensions{96U, 64U};
    auto source = smooth_surface(dimensions);
    image::SensorClippingMask clipping{
        .dimensions = dimensions,
        .samples = std::vector<std::uint8_t>(static_cast<std::size_t>(dimensions.pixel_count())),
    };
    for (std::uint32_t y = 22U; y <= 42U; ++y) {
        for (std::uint32_t x = 38U; x <= 58U; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * dimensions.width + x;
            clipping.samples[pixel] = image::sensor_highlight_clipped;
            ++clipping.highlight_pixel_count;
            const auto sample = pixel * 3U;
            source.samples[sample] = 2.8F;
            source.samples[sample + 1U] = 0.62F;
            source.samples[sample + 2U] = 1.9F;
        }
    }
    const auto before = source;
    auto risk = highlight_risk_from_mask(clipping);

    static_cast<void>(
        image::raw_pipeline_detail::reconstruct_clipped_highlight_surface(source, clipping, risk)
    );

    expect(
        maximum_luminance_change(source, before) < 2.0e-5,
        "single/two-channel physical clipping may repair chroma but cannot invent a low-frequency "
        "luminance surface"
    );
}

void contradictory_cfa_core_follows_the_measured_boundary_without_a_magenta_core() {
    constexpr image::Dimensions dimensions{128U, 80U};
    constexpr std::uint32_t centre_x = 66U;
    constexpr std::uint32_t centre_y = 40U;
    auto source = image::SceneLinearRgbFrame{
        .dimensions = dimensions,
        .row_stride_bytes = static_cast<std::size_t>(dimensions.width) * 3U * sizeof(float),
        .samples = std::vector<float>(static_cast<std::size_t>(dimensions.pixel_count()) * 3U),
    };
    for (std::size_t pixel = 0U; pixel < dimensions.pixel_count(); ++pixel) {
        const auto sample = pixel * 3U;
        source.samples[sample] = 0.92F;
        source.samples[sample + 1U] = 0.70F;
        source.samples[sample + 2U] = 0.42F;
    }
    auto clipping = circular_clipping_mask(dimensions, centre_x, centre_y, 16U);
    auto risk = highlight_risk_from_mask(clipping);
    for (std::uint32_t y = 19U; y <= 61U; ++y) {
        for (std::uint32_t x = 43U; x <= 89U; ++x) {
            const auto pixel = static_cast<std::size_t>(y) * dimensions.width + x;
            const auto sample = pixel * 3U;
            const auto dx = static_cast<std::int64_t>(x) - centre_x;
            const auto dy = static_cast<std::int64_t>(y) - centre_y;
            const auto distance_squared = dx * dx + dy * dy;
            if ((clipping.samples[pixel] & image::sensor_highlight_clipped) != 0U) {
                source.samples[sample] = 3.4F;
                source.samples[sample + 1U] = 0.35F;
                source.samples[sample + 2U] = 2.8F;
            } else if (distance_squared <= 21 * 21) {
                // Model an unclipped but CFA-risked colour fringe surrounding the core. It is
                // bright enough to dominate a naïve guide, yet is explicitly not reliable
                // evidence for the missing surface.
                source.samples[sample] = 2.8F;
                source.samples[sample + 1U] = 0.40F;
                source.samples[sample + 2U] = 2.4F;
                risk.samples[pixel] = 255U;
            }
        }
    }

    static_cast<void>(
        image::raw_pipeline_detail::reconstruct_clipped_highlight_surface(source, clipping, risk)
    );

    const auto centre_pixel = static_cast<std::size_t>(centre_y) * dimensions.width + centre_x;
    const auto centre_sample = centre_pixel * 3U;
    const float red_blue_ratio =
        source.samples[centre_sample] / std::max(source.samples[centre_sample + 2U], 1.0e-6F);
    const float green_blue_ratio =
        source.samples[centre_sample + 1U] / std::max(source.samples[centre_sample + 2U], 1.0e-6F);
    constexpr float expected_red_blue_ratio = 0.92F / 0.42F;
    constexpr float expected_green_blue_ratio = 0.70F / 0.42F;
    if (!(std::abs(red_blue_ratio - expected_red_blue_ratio) < 0.15F
          && std::abs(green_blue_ratio - expected_green_blue_ratio) < 0.15F)) {
        std::cerr << "diagnostic: contradictory core ratios red/blue=" << red_blue_ratio
                  << " green/blue=" << green_blue_ratio << '\n';
    }
    expect(
        std::abs(red_blue_ratio - expected_red_blue_ratio) < 0.15F
            && std::abs(green_blue_ratio - expected_green_blue_ratio) < 0.15F,
        "a contradictory core adopts the reliable warm boundary instead of retaining the "
        "CFA-risked magenta ratio or inventing display grey"
    );
    expect(
        risk.samples[centre_pixel] == 64U,
        "a fully reconstructed core retains only bounded terminal chroma uncertainty"
    );
}

void empty_or_unrecoverable_masks_are_no_ops() {
    constexpr image::Dimensions dimensions{11U, 7U};
    auto source = smooth_surface(dimensions);
    const auto original = source.samples;
    image::SensorClippingMask empty{
        .dimensions = dimensions,
        .samples = std::vector<std::uint8_t>(static_cast<std::size_t>(dimensions.pixel_count())),
    };
    auto empty_risk = highlight_risk_from_mask(empty);
    const auto empty_stats = image::raw_pipeline_detail::reconstruct_clipped_highlight_surface(
        source,
        empty,
        empty_risk
    );
    expect(
        empty_stats.blended_pixel_count == 0U && source.samples == original,
        "a source without physical clipping remains untouched"
    );

    image::SensorClippingMask full{
        .dimensions = dimensions,
        .samples = std::vector<std::uint8_t>(
            static_cast<std::size_t>(dimensions.pixel_count()),
            image::sensor_highlight_clipped
        ),
        .highlight_pixel_count = dimensions.pixel_count(),
    };
    auto full_risk = highlight_risk_from_mask(full);
    const auto full_stats =
        image::raw_pipeline_detail::reconstruct_clipped_highlight_surface(source, full, full_risk);
    expect(
        full_stats.blended_pixel_count == 0U && source.samples == original,
        "an entirely clipped source is left factual when no measured boundary exists"
    );
}

} // namespace

int main() {
    cfa_completion_retains_only_bounded_terminal_uncertainty();
    reconstruction_hides_clip_topology_without_crossing_dark_edges();
    measured_highlight_shoulder_remains_exact();
    cfa_risk_bands_do_not_modulate_the_luminance_feather();
    reconstruction_preserves_bright_warm_highlight_character();
    sparse_projected_cfa_clip_does_not_invent_an_rgb_surface();
    single_channel_clipping_cannot_become_a_luminance_surface();
    contradictory_cfa_core_follows_the_measured_boundary_without_a_magenta_core();
    empty_or_unrecoverable_masks_are_no_ops();
    if (failures != 0) {
        std::cerr << failures << " clipped-highlight reconstruction contract test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "clipped-highlight reconstruction contracts passed\n";
    return EXIT_SUCCESS;
}

// Included once inside cpu_reference.cpp's private namespace.
// Frequency detail, sharpening, denoise, dehaze/defringe, grading, grain and
// vignette share one staged Detail & Effects parameter contract.

[[nodiscard]] float checked_float(
    const double value,
    const std::size_t node_index,
    const AdjustmentNode& node
) {
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(value) || value < -maximum || value > maximum) {
        throw_node_error(
            EditErrorCode::numeric_overflow,
            node_index,
            node,
            "pixel result exceeded finite float32 range"
        );
    }
    return static_cast<float>(value);
}

template <typename Transform>
void transform_rgb_pixels(
    FloatRgbImage& image,
    const std::size_t node_index,
    const AdjustmentNode& node,
    Transform&& transform
) {
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    // Oklab/OKLCH operations are independent per pixel. Use the process-wide bounded image
    // executor so a graph with several active nodes does not repeatedly create and destroy
    // thread groups or oversubscribe the machine.
    shadow::image::detail::parallel_for_rows(
        image.dimensions.height,
        32U,
        [
        &image,
        stride,
        node_index,
        &node,
        transform = std::forward<Transform>(transform)
    ](const std::uint32_t first_row, const std::uint32_t past_last_row) mutable {
        for (std::uint32_t y = first_row; y < past_last_row; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * stride;
            for (std::uint32_t x = 0; x < image.dimensions.width; ++x) {
                const std::size_t sample = row + static_cast<std::size_t>(x) * rgb_channels;
                const std::array<double, 3> input{
                    static_cast<double>(image.samples[sample]),
                    static_cast<double>(image.samples[sample + 1U]),
                    static_cast<double>(image.samples[sample + 2U]),
                };
                const std::array<double, 3> output = transform(input);
                image.samples[sample] = checked_float(output[0], node_index, node);
                image.samples[sample + 1U] = checked_float(output[1], node_index, node);
                image.samples[sample + 2U] = checked_float(output[2], node_index, node);
            }
        }
        }
    );
}

// Neighbourhood operators materialize intermediate fields, but every row of each separable pass
// remains independent. Route them through the same bounded executor as pixel-local operations.
template <typename Work>
void parallel_for_rows(const std::size_t height, Work&& work) {
    if (height > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("image height exceeds the row scheduler contract");
    }
    shadow::image::detail::parallel_for_rows(
        static_cast<std::uint32_t>(height),
        32U,
        std::forward<Work>(work)
    );
}

[[nodiscard]] std::size_t reflect101_index(
    std::int64_t index,
    const std::size_t extent
) noexcept {
    if (extent <= 1U) {
        return 0U;
    }
    const auto signed_extent = static_cast<std::int64_t>(extent);
    while (index < 0 || index >= signed_extent) {
        if (index < 0) {
            index = -index;
        } else {
            index = 2 * signed_extent - 2 - index;
        }
    }
    return static_cast<std::size_t>(index);
}

[[nodiscard]] std::vector<double> gaussian_kernel(
    const double sigma,
    const std::uint32_t radius
) {
    const std::size_t size = static_cast<std::size_t>(radius) * 2U + 1U;
    std::vector<double> kernel(size);
    const double inverse_two_sigma_squared = 1.0 / (2.0 * sigma * sigma);
    double sum = 0.0;
    for (std::int64_t offset = -static_cast<std::int64_t>(radius);
         offset <= static_cast<std::int64_t>(radius);
         ++offset) {
        const double coordinate = static_cast<double>(offset);
        const double value = std::exp(-(coordinate * coordinate) * inverse_two_sigma_squared);
        kernel[static_cast<std::size_t>(offset + static_cast<std::int64_t>(radius))] = value;
        sum += value;
    }
    for (double& value : kernel) {
        value /= sum;
    }
    return kernel;
}

// Blur one scalar image with a separable Gaussian. This intentionally keeps
// the detail operator in a single scalar (Oklab-L) field: processing RGB
// channels independently makes the ordinary clarity/texture controls create
// color halos, which is precisely what the perceptual architecture avoids.
[[nodiscard]] std::vector<double> gaussian_blur_scalar(
    const std::vector<double>& source,
    const std::size_t width,
    const std::size_t height,
    const double sigma_x,
    const double sigma_y
) {
    const std::uint32_t radius_x = static_cast<std::uint32_t>(std::max(
        1.0,
        std::ceil(3.0 * sigma_x)
    ));
    const std::uint32_t radius_y = static_cast<std::uint32_t>(std::max(
        1.0,
        std::ceil(3.0 * sigma_y)
    ));
    const auto kernel_x = gaussian_kernel(std::max(0.20, sigma_x), radius_x);
    const auto kernel_y = gaussian_kernel(std::max(0.20, sigma_y), radius_y);
    std::vector<double> horizontal(source.size());
    std::vector<double> result(source.size());
    const auto signed_radius_x = static_cast<std::int64_t>(radius_x);
    const auto signed_radius_y = static_cast<std::int64_t>(radius_y);
    parallel_for_rows(height, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
        for (std::size_t y = first_row; y < past_last_row; ++y) {
            for (std::size_t x = 0U; x < width; ++x) {
                double sum = 0.0;
                for (std::int64_t offset = -signed_radius_x;
                     offset <= signed_radius_x;
                     ++offset) {
                    const std::size_t source_x = reflect101_index(
                        static_cast<std::int64_t>(x) + offset,
                        width
                    );
                    sum += source[y * width + source_x]
                        * kernel_x[static_cast<std::size_t>(offset + signed_radius_x)];
                }
                horizontal[y * width + x] = sum;
            }
        }
    });
    parallel_for_rows(height, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
        for (std::size_t y = first_row; y < past_last_row; ++y) {
            for (std::size_t x = 0U; x < width; ++x) {
                double sum = 0.0;
                for (std::int64_t offset = -signed_radius_y;
                     offset <= signed_radius_y;
                     ++offset) {
                    const std::size_t source_y = reflect101_index(
                        static_cast<std::int64_t>(y) + offset,
                        height
                    );
                    sum += horizontal[source_y * width + x]
                        * kernel_y[static_cast<std::size_t>(offset + signed_radius_y)];
                }
                result[y * width + x] = sum;
            }
        }
    });
    return result;
}

// The perceptual detail section deliberately separates two bands rather than
// reusing capture sharpening: Texture is the compact high-frequency residual,
// while Clarity is a protected mid-frequency residual. Both alter Oklab L only
// so their visible effect is lightness/structure, not a channel-wise RGB
// contrast shift. A high-frequency edge guard suppresses large-scale Gaussian
// haloing at hard edges; the final output is still handled by the common gamut
// mapper, after every creative node has run.
void apply_perceptual_detail(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SharpenAdjustment& parameters
) {
    if (parameters.clarity == 0.0 && parameters.texture == 0.0) {
        return;
    }
    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        throw_node_error(
            EditErrorCode::numeric_overflow,
            node_index,
            node,
            "perceptual detail working buffer exceeds the address space"
        );
    }
    const std::size_t pixels = width * height;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    const WorkingSpaceTransform color_transform = prepare_working_space_transform(
        image.working_space, node, node_index
    );
    std::vector<Vector3> oklab(pixels);
    std::vector<double> lightness(pixels);
    parallel_for_rows(height, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
        for (std::size_t y = first_row; y < past_last_row; ++y) {
            const std::size_t row = y * stride;
            for (std::size_t x = 0U; x < width; ++x) {
                const std::size_t pixel = y * width + x;
                const std::size_t sample = row + x * rgb_channels;
                oklab[pixel] = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, Vector3{
                    static_cast<double>(image.samples[sample]),
                    static_cast<double>(image.samples[sample + 1U]),
                    static_cast<double>(image.samples[sample + 2U]),
                }));
                lightness[pixel] = oklab[pixel][0];
            }
        }
    });

    // All radii are expressed in level-zero/native pixels, exactly like the
    // technical detail controls. This gives a warm proxy and a full-resolution
    // export the same physical interpretation rather than a proxy-dependent
    // clarity look.
    constexpr double texture_sigma_level_zero = 1.4;
    constexpr double clarity_small_sigma_level_zero = 2.4;
    constexpr double clarity_large_sigma_level_zero = 12.0;
    const double texture_sigma_x = texture_sigma_level_zero
        * image.level_zero_to_raster_scale_x;
    const double texture_sigma_y = texture_sigma_level_zero
        * image.level_zero_to_raster_scale_y;
    const double clarity_small_sigma_x = clarity_small_sigma_level_zero
        * image.level_zero_to_raster_scale_x;
    const double clarity_small_sigma_y = clarity_small_sigma_level_zero
        * image.level_zero_to_raster_scale_y;
    const double clarity_large_sigma_x = clarity_large_sigma_level_zero
        * image.level_zero_to_raster_scale_x;
    const double clarity_large_sigma_y = clarity_large_sigma_level_zero
        * image.level_zero_to_raster_scale_y;

    std::vector<double> texture_base;
    std::vector<double> clarity_small;
    std::vector<double> clarity_large;
    if (parameters.texture != 0.0) {
        texture_base = gaussian_blur_scalar(
            lightness, width, height, texture_sigma_x, texture_sigma_y
        );
    }
    if (parameters.clarity != 0.0) {
        clarity_small = gaussian_blur_scalar(
            lightness, width, height, clarity_small_sigma_x, clarity_small_sigma_y
        );
        clarity_large = gaussian_blur_scalar(
            lightness, width, height, clarity_large_sigma_x, clarity_large_sigma_y
        );
    }

    const auto compress_detail = [](const double value, const double knee) noexcept {
        return value / (1.0 + std::abs(value) / knee);
    };
    parallel_for_rows(height, [&](const std::uint32_t first_row, const std::uint32_t past_last_row) {
        for (std::size_t y = first_row; y < past_last_row; ++y) {
            const std::size_t row = y * stride;
            for (std::size_t x = 0U; x < width; ++x) {
                const std::size_t pixel = y * width + x;
                Vector3 output_lab = oklab[pixel];
            // Avoid exposing unstable residuals in the near-black toe, while
            // allowing a negative value to soften detail as naturally as a
            // positive value enhances it.
            const double shadow_protection = smoothstep(0.015, 0.090, output_lab[0]);
            if (parameters.texture != 0.0) {
                const double residual = output_lab[0] - texture_base[pixel];
                output_lab[0] += parameters.texture * 0.70
                    * compress_detail(residual, 0.035) * shadow_protection;
            }
            if (parameters.clarity != 0.0) {
                const double high_frequency = output_lab[0] - clarity_small[pixel];
                const double mid_frequency = clarity_small[pixel] - clarity_large[pixel];
                // A broad Gaussian cannot follow a hard edge. Fade that band
                // there so the operator improves local structure rather than
                // producing a dark/light outline around high-contrast edges.
                const double edge_protection = 1.0 - smoothstep(
                    0.018,
                    0.085,
                    std::abs(high_frequency)
                );
                output_lab[0] += parameters.clarity * 1.15
                    * compress_detail(mid_frequency, 0.090)
                    * edge_protection * shadow_protection;
            }
            const Vector3 output = multiply(color_transform.xyz_to_rgb, oklab_to_xyz(output_lab));
            const std::size_t sample = row + x * rgb_channels;
            image.samples[sample] = checked_float(output[0], node_index, node);
            image.samples[sample + 1U] = checked_float(output[1], node_index, node);
                image.samples[sample + 2U] = checked_float(output[2], node_index, node);
            }
        }
    });
}

void apply_sharpen(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SharpenAdjustment& parameters
) {
    if (parameters.amount == 0.0) {
        return;
    }

    const AdjustmentFootprint support = footprint(
        parameters,
        image.level_zero_to_raster_scale_x,
        image.level_zero_to_raster_scale_y
    );
    const double sigma_x = parameters.radius * image.level_zero_to_raster_scale_x;
    const double sigma_y = parameters.radius * image.level_zero_to_raster_scale_y;
    const auto kernel_x = gaussian_kernel(sigma_x, support.horizontal_radius);
    const auto kernel_y = gaussian_kernel(sigma_y, support.vertical_radius);

    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    if (width > std::numeric_limits<std::size_t>::max() / height) {
        throw_node_error(
            EditErrorCode::numeric_overflow,
            node_index,
            node,
            "sharpen working buffer exceeds the address space"
        );
    }
    const std::size_t pixels = width * height;
    std::vector<double> log_luminance(pixels);
    std::vector<double> horizontal_blur(pixels);
    std::vector<double> blurred(pixels);
    const auto weights = image.working_space.luminance_coefficients;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    constexpr double minimum_positive_luminance = 5.9604644775390625e-8; // 2^-24

    for (std::size_t y = 0U; y < height; ++y) {
        const std::size_t row = y * stride;
        for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t sample = row + x * rgb_channels;
            const double luminance = static_cast<double>(image.samples[sample]) * weights[0]
                + static_cast<double>(image.samples[sample + 1U]) * weights[1]
                + static_cast<double>(image.samples[sample + 2U]) * weights[2];
            log_luminance[y * width + x] = std::log2(
                std::max(luminance, minimum_positive_luminance)
            );
        }
    }

    const auto radius_x = static_cast<std::int64_t>(support.horizontal_radius);
    const auto radius_y = static_cast<std::int64_t>(support.vertical_radius);
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            double sum = 0.0;
            for (std::int64_t offset = -radius_x; offset <= radius_x; ++offset) {
                const std::size_t source_x = reflect101_index(
                    static_cast<std::int64_t>(x) + offset,
                    width
                );
                sum += log_luminance[y * width + source_x]
                    * kernel_x[static_cast<std::size_t>(offset + radius_x)];
            }
            horizontal_blur[y * width + x] = sum;
        }
    }
    for (std::size_t y = 0U; y < height; ++y) {
        for (std::size_t x = 0U; x < width; ++x) {
            double sum = 0.0;
            for (std::int64_t offset = -radius_y; offset <= radius_y; ++offset) {
                const std::size_t source_y = reflect101_index(
                    static_cast<std::int64_t>(y) + offset,
                    height
                );
                sum += horizontal_blur[source_y * width + x]
                    * kernel_y[static_cast<std::size_t>(offset + radius_y)];
            }
            blurred[y * width + x] = sum;
        }
    }

    const double threshold_ev = parameters.threshold * 0.25;
    for (std::size_t y = 0U; y < height; ++y) {
        const std::size_t row = y * stride;
        for (std::size_t x = 0U; x < width; ++x) {
            const std::size_t pixel = y * width + x;
            const std::size_t sample = row + x * rgb_channels;
            const double luminance = static_cast<double>(image.samples[sample]) * weights[0]
                + static_cast<double>(image.samples[sample + 1U]) * weights[1]
                + static_cast<double>(image.samples[sample + 2U]) * weights[2];
            if (luminance <= minimum_positive_luminance) {
                continue;
            }

            const double detail = log_luminance[pixel] - blurred[pixel];
            // The tiny guard makes a mathematically flat field exactly neutral despite the
            // unavoidable roundoff of a normalized separable convolution.
            if (std::abs(detail) <= 1.0e-12) {
                continue;
            }
            const double thresholded = std::copysign(
                std::max(0.0, std::abs(detail) - threshold_ev),
                detail
            );
            if (thresholded == 0.0) {
                continue;
            }
            const double edge_confidence = smoothstep(
                threshold_ev,
                threshold_ev + 0.25,
                std::abs(detail)
            );
            const double mask = (1.0 - parameters.masking)
                + parameters.masking * edge_confidence;
            const double gain = std::exp2(parameters.amount * thresholded * mask);
            image.samples[sample] = checked_float(
                static_cast<double>(image.samples[sample]) * gain,
                node_index,
                node
            );
            image.samples[sample + 1U] = checked_float(
                static_cast<double>(image.samples[sample + 1U]) * gain,
                node_index,
                node
            );
            image.samples[sample + 2U] = checked_float(
                static_cast<double>(image.samples[sample + 2U]) * gain,
                node_index,
                node
            );
        }
    }
}

void apply_edge_aware_denoise(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SharpenAdjustment& parameters
) {
    if (parameters.denoise_luminance == 0.0 && parameters.denoise_color == 0.0) {
        return;
    }
    const std::size_t width = image.dimensions.width;
    const std::size_t height = image.dimensions.height;
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    const auto source = image.samples;
    const auto weights = image.working_space.luminance_coefficients;
    const double range_sigma = 0.025 + 0.18 * (1.0 - parameters.denoise_detail);
    const double inverse_range = 1.0 / (2.0 * range_sigma * range_sigma);
    // The UI defines detail radius in level-zero (native RAW) pixels. A fixed 5x5 kernel on a
    // 1200px warm proxy would otherwise denoise a much larger physical region than the same
    // setting on a full-detail tile. Preserve the native sigma, then convert it separately to
    // each raster axis just as capture sharpening already does.
    constexpr double denoise_native_sigma = 1.5;
    constexpr double denoise_native_support = 2.0;
    const double sigma_x = std::max(
        0.20,
        denoise_native_sigma * image.level_zero_to_raster_scale_x
    );
    const double sigma_y = std::max(
        0.20,
        denoise_native_sigma * image.level_zero_to_raster_scale_y
    );
    const std::int64_t radius_x = std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(std::ceil(
            denoise_native_support * image.level_zero_to_raster_scale_x
        ))
    );
    const std::int64_t radius_y = std::max<std::int64_t>(
        1,
        static_cast<std::int64_t>(std::ceil(
            denoise_native_support * image.level_zero_to_raster_scale_y
        ))
    );
    const double inverse_two_sigma_x_squared = 1.0 / (2.0 * sigma_x * sigma_x);
    const double inverse_two_sigma_y_squared = 1.0 / (2.0 * sigma_y * sigma_y);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t center = y * stride + x * rgb_channels;
            const Vector3 original{
                source[center], source[center + 1U], source[center + 2U],
            };
            const double original_luma = original[0] * weights[0]
                + original[1] * weights[1] + original[2] * weights[2];
            Vector3 filtered{};
            double weight_sum = 0.0;
            for (std::int64_t dy = -radius_y; dy <= radius_y; ++dy) {
                const std::size_t source_y = reflect101_index(
                    static_cast<std::int64_t>(y) + dy, height
                );
                for (std::int64_t dx = -radius_x; dx <= radius_x; ++dx) {
                    const std::size_t source_x = reflect101_index(
                        static_cast<std::int64_t>(x) + dx, width
                    );
                    const std::size_t sample = source_y * stride + source_x * rgb_channels;
                    const Vector3 neighbor{
                        source[sample], source[sample + 1U], source[sample + 2U],
                    };
                    const double neighbor_luma = neighbor[0] * weights[0]
                        + neighbor[1] * weights[1] + neighbor[2] * weights[2];
                    const double delta = neighbor_luma - original_luma;
                    const double spatial = static_cast<double>(dx * dx)
                            * inverse_two_sigma_x_squared
                        + static_cast<double>(dy * dy) * inverse_two_sigma_y_squared;
                    const double weight = std::exp(-spatial - delta * delta * inverse_range);
                    for (std::size_t channel = 0; channel < rgb_channels; ++channel) {
                        filtered[channel] += neighbor[channel] * weight;
                    }
                    weight_sum += weight;
                }
            }
            for (double& channel : filtered) {
                channel /= weight_sum;
            }
            const double filtered_luma = filtered[0] * weights[0]
                + filtered[1] * weights[1] + filtered[2] * weights[2];
            const double luminance = std::lerp(
                original_luma, filtered_luma, parameters.denoise_luminance
            );
            for (std::size_t channel = 0; channel < rgb_channels; ++channel) {
                const double original_chroma = original[channel] - original_luma;
                const double filtered_chroma = filtered[channel] - filtered_luma;
                const double output = luminance + std::lerp(
                    original_chroma, filtered_chroma, parameters.denoise_color
                );
                image.samples[center + channel] = checked_float(output, node_index, node);
            }
        }
    }
}

void apply_dehaze_and_defringe(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SharpenAdjustment& parameters
) {
    if (parameters.dehaze == 0.0
        && parameters.defringe_purple_amount == 0.0
        && parameters.defringe_green_amount == 0.0) {
        return;
    }
    const auto luma_weights = image.working_space.luminance_coefficients;
    const WorkingSpaceTransform color_transform = prepare_working_space_transform(
        image.working_space, node, node_index
    );
    transform_rgb_pixels(
        image,
        node_index,
        node,
        [&parameters, luma_weights, &color_transform](Vector3 input) {
            const double luma = input[0] * luma_weights[0]
                + input[1] * luma_weights[1] + input[2] * luma_weights[2];
            if (parameters.dehaze > 0.0) {
                const double veil = std::max(0.0, std::min({input[0], input[1], input[2]}));
                const double maximum = std::max({0.0, input[0], input[1], input[2]});
                const double veil_fraction = std::clamp(veil / (maximum + 0.18), 0.0, 1.0);
                const double transmission = std::max(
                    0.2, 1.0 - 0.88 * parameters.dehaze * veil_fraction
                );
                for (double& channel : input) {
                    channel = (channel - parameters.dehaze * 0.65 * veil) / transmission;
                }
            } else if (parameters.dehaze < 0.0) {
                const double amount = -parameters.dehaze;
                const double atmosphere = std::max(0.18, luma + 0.28);
                for (double& channel : input) {
                    channel = std::lerp(channel, atmosphere, 0.55 * amount);
                }
            }

            Vector3 lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, input));
            const double chroma = std::hypot(lab[1], lab[2]);
            if ((parameters.defringe_purple_amount > 0.0
                    || parameters.defringe_green_amount > 0.0)
                && chroma > 1.0e-8) {
                const double hue = wrap_degrees(std::atan2(lab[2], lab[1]) * 180.0 / pi);
                constexpr double range_feather_degrees = 10.0;
                const auto range_weight = [hue](const double low, const double high) {
                    return smoothstep(low - range_feather_degrees, low, hue)
                        * (1.0 - smoothstep(high, high + range_feather_degrees, hue));
                };
                const double purple = parameters.defringe_purple_amount * range_weight(
                    parameters.defringe_purple_hue_low,
                    parameters.defringe_purple_hue_high
                );
                const double green = parameters.defringe_green_amount * range_weight(
                    parameters.defringe_green_hue_low,
                    parameters.defringe_green_hue_high
                );
                const double reduction = std::max(purple, green);
                lab[1] *= 1.0 - 0.9 * reduction;
                lab[2] *= 1.0 - 0.9 * reduction;
            }

            return multiply(color_transform.xyz_to_rgb, oklab_to_xyz(lab));
        }
    );
}

struct PreparedColorGradingWheel final {
    double delta_a = 0.0;
    double delta_b = 0.0;
    double delta_lightness = 0.0;
};

struct PreparedColorGrading final {
    PreparedColorGradingWheel shadows;
    PreparedColorGradingWheel midtones;
    PreparedColorGradingWheel highlights;
    double center = 0.5;
    double width = 0.23;
};

[[nodiscard]] PreparedColorGrading prepare_color_grading(
    const SharpenAdjustment& parameters
) noexcept {
    const auto prepare_wheel = [](const double hue, const double saturation,
                                  const double luminance) {
        const double angle = hue * pi / 180.0;
        return PreparedColorGradingWheel{
            .delta_a = 0.09 * saturation * std::cos(angle),
            .delta_b = 0.09 * saturation * std::sin(angle),
            .delta_lightness = 0.12 * luminance,
        };
    };
    return PreparedColorGrading{
        .shadows = prepare_wheel(
            parameters.shadows_hue,
            parameters.shadows_saturation,
            parameters.shadows_luminance
        ),
        .midtones = prepare_wheel(
            parameters.midtones_hue,
            parameters.midtones_saturation,
            parameters.midtones_luminance
        ),
        .highlights = prepare_wheel(
            parameters.highlights_hue,
            parameters.highlights_saturation,
            parameters.highlights_luminance
        ),
        .center = std::clamp(
            0.5 + 0.22 * parameters.grading_balance,
            0.18,
            0.82
        ),
        .width = 0.08 + 0.30 * parameters.grading_blending,
    };
}

void apply_color_grading(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SharpenAdjustment& parameters
) {
    const bool grading = parameters.shadows_saturation != 0.0
        || parameters.shadows_luminance != 0.0
        || parameters.midtones_saturation != 0.0
        || parameters.midtones_luminance != 0.0
        || parameters.highlights_saturation != 0.0
        || parameters.highlights_luminance != 0.0;
    if (!grading) {
        return;
    }
    const PreparedColorGrading prepared = prepare_color_grading(parameters);
    const auto luma_weights = image.working_space.luminance_coefficients;
    const WorkingSpaceTransform color_transform = prepare_working_space_transform(
        image.working_space, node, node_index
    );
    transform_rgb_pixels(
        image,
        node_index,
        node,
        [&prepared, luma_weights, &color_transform](const Vector3& input) {
            const double luma = input[0] * luma_weights[0]
                + input[1] * luma_weights[1] + input[2] * luma_weights[2];
            Vector3 lab = xyz_to_oklab(multiply(color_transform.rgb_to_xyz, input));
            const double normalized = std::max(0.0, luma) / (std::max(0.0, luma) + 0.18);
            double shadow_weight = 1.0 - smoothstep(
                prepared.center - prepared.width,
                prepared.center + prepared.width,
                normalized
            );
            double highlight_weight = smoothstep(
                prepared.center - prepared.width,
                prepared.center + prepared.width,
                normalized
            );
            double midtone_weight = 1.0 - std::abs(normalized - prepared.center)
                / std::max(0.12, 0.5 + prepared.width);
            midtone_weight = std::clamp(midtone_weight, 0.0, 1.0);
            const double total = shadow_weight + midtone_weight + highlight_weight;
            shadow_weight /= total;
            midtone_weight /= total;
            highlight_weight /= total;
            const auto apply_wheel = [&lab](
                const PreparedColorGradingWheel& wheel,
                const double weight
            ) {
                lab[0] += wheel.delta_lightness * weight;
                lab[1] += wheel.delta_a * weight;
                lab[2] += wheel.delta_b * weight;
            };
            apply_wheel(prepared.shadows, shadow_weight);
            apply_wheel(prepared.midtones, midtone_weight);
            apply_wheel(prepared.highlights, highlight_weight);
            return multiply(color_transform.xyz_to_rgb, oklab_to_xyz(lab));
        }
    );
}

[[nodiscard]] double coordinate_noise(std::uint32_t x, std::uint32_t y, std::uint32_t seed) noexcept {
    std::uint32_t value = x * 0x9e3779b9U ^ y * 0x85ebca6bU ^ seed;
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return static_cast<double>(value) / static_cast<double>(std::numeric_limits<std::uint32_t>::max())
        * 2.0 - 1.0;
}

void apply_grain_and_vignette(
    FloatRgbImage& image,
    const AdjustmentNode& node,
    const std::size_t node_index,
    const SharpenAdjustment& parameters,
    const AdjustmentExecutionContext& context
) {
    if (parameters.grain_amount == 0.0 && parameters.vignette_amount == 0.0) {
        return;
    }
    const std::size_t stride = image.row_stride_bytes / sizeof(float);
    const auto weights = image.working_space.luminance_coefficients;
    const double full_width = context.full_dimensions.width;
    const double full_height = context.full_dimensions.height;
    const std::uint32_t grain_block = 1U + static_cast<std::uint32_t>(
        std::round(parameters.grain_size * 3.0)
    );
    for (std::uint32_t y = 0; y < image.dimensions.height; ++y) {
        for (std::uint32_t x = 0; x < image.dimensions.width; ++x) {
            const std::size_t sample = static_cast<std::size_t>(y) * stride
                + static_cast<std::size_t>(x) * rgb_channels;
            const std::uint32_t global_x = context.origin_x + x;
            const std::uint32_t global_y = context.origin_y + y;
            const double luma = image.samples[sample] * weights[0]
                + image.samples[sample + 1U] * weights[1]
                + image.samples[sample + 2U] * weights[2];
            double gain = 1.0;
            double additive = 0.0;
            if (parameters.grain_amount > 0.0) {
                const double coarse = coordinate_noise(
                    global_x / grain_block, global_y / grain_block, 0x51ed270bU
                );
                const double fine = coordinate_noise(global_x, global_y, 0xa54ff53aU);
                const double noise = std::lerp(coarse, fine, parameters.grain_roughness);
                const double visibility = 0.45 + 0.55 * (1.0 - smoothstep(0.0, 1.0, luma));
                additive = noise * parameters.grain_amount
                    * (0.012 + 0.035 * parameters.grain_roughness) * visibility;
            }
            if (parameters.vignette_amount != 0.0) {
                double nx = (static_cast<double>(global_x) + 0.5) / full_width * 2.0 - 1.0;
                double ny = (static_cast<double>(global_y) + 0.5) / full_height * 2.0 - 1.0;
                nx *= full_width / std::max(full_width, full_height);
                ny *= full_height / std::max(full_width, full_height);
                const double circle = std::hypot(nx, ny);
                const double square = std::max(std::abs(nx), std::abs(ny));
                const double round_mix = 0.5 * (parameters.vignette_roundness + 1.0);
                const double radius = std::lerp(square, circle, round_mix);
                const double start = 0.15 + 0.65 * parameters.vignette_midpoint;
                const double feather = 0.04 + 0.50 * parameters.vignette_feather;
                const double mask = smoothstep(start, start + feather, radius);
                double stops = 2.0 * parameters.vignette_amount * mask;
                if (stops < 0.0) {
                    const double highlight = smoothstep(0.6, 1.6, luma);
                    stops *= 1.0 - parameters.vignette_highlights * highlight;
                }
                gain = std::exp2(stops);
            }
            for (std::size_t channel = 0; channel < rgb_channels; ++channel) {
                image.samples[sample + channel] = checked_float(
                    static_cast<double>(image.samples[sample + channel]) * gain + additive,
                    node_index,
                    node
                );
            }
        }
    }
}

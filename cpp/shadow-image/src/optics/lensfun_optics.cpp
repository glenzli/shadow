#include <shadow/image/optics.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef SHADOW_IMAGE_HAS_LENSFUN
#define SHADOW_IMAGE_HAS_LENSFUN 0
#endif

#if SHADOW_IMAGE_HAS_LENSFUN
#include <lensfun/lensfun.h>
#endif

namespace shadow::image {

namespace {

[[nodiscard]] OpticsProfileReceipt unavailable_receipt(
    const OpticsProviderInfo& info,
    const OpticsProfileStatus status
) {
    return OpticsProfileReceipt{
        .status = status,
        .provider_id = info.id,
        .provider_version = info.version,
    };
}

void validate_settings(const OpticsSettings& settings) {
    if (settings.schema_version != optics_settings_schema_version) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "unsupported optics settings schema version"
        );
    }
    const bool has_manual_camera = !settings.camera_profile_model.empty();
    const bool has_manual_lens = !settings.lens_profile_model.empty();
    if (has_manual_camera != has_manual_lens) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "manual optics selection requires both camera and lens profile models"
        );
    }
}

#if SHADOW_IMAGE_HAS_LENSFUN

constexpr std::size_t rgb_channels = 3U;
constexpr std::uint32_t remap_rows_per_batch = 48U;

[[nodiscard]] bool finite_positive(const double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] std::string trim_ascii(const std::string_view value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(begin, end - begin + 1U));
}

void validate_input(const PixelBuffer& input) {
    if (
        input.dimensions.width == 0U || input.dimensions.height == 0U
        || input.bits_per_channel != 16U || input.channels != rgb_channels
        || input.transfer_function != RgbTransferFunction::linear
        || input.primaries != RgbPrimaries::srgb_rec709_d65
        || input.reference != RgbBufferReference::processed_raw
    ) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "optics expects linear processed 16-bit sRGB-primary RGB"
        );
    }
    const auto width = static_cast<std::size_t>(input.dimensions.width);
    const auto height = static_cast<std::size_t>(input.dimensions.height);
    if (
        width > std::numeric_limits<std::size_t>::max() / rgb_channels
        || height > std::numeric_limits<std::size_t>::max() / (width * rgb_channels)
    ) {
        throw DecodeError(
            DecodeErrorCode::resource_limit,
            0,
            "optics input dimensions overflow the address space"
        );
    }
    const auto expected_samples = width * height * rgb_channels;
    if (
        input.row_stride_bytes != width * rgb_channels * sizeof(std::uint16_t)
        || input.samples.size() != expected_samples
    ) {
        throw DecodeError(
            DecodeErrorCode::corrupt_data,
            0,
            "optics input layout does not match its RGB descriptor"
        );
    }
}

template <typename Sample>
class AlignedSamples final {
public:
    explicit AlignedSamples(const std::size_t count) : count_(count) {
        if (count_ == 0U || count_ > std::numeric_limits<std::size_t>::max() / sizeof(Sample)) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "optics aligned working buffer is too large"
            );
        }
        data_ = static_cast<Sample*>(
            ::operator new(count_ * sizeof(Sample), std::align_val_t{16U})
        );
    }

    AlignedSamples(const AlignedSamples&) = delete;
    AlignedSamples& operator=(const AlignedSamples&) = delete;

    ~AlignedSamples() {
        ::operator delete(data_, std::align_val_t{16U});
    }

    [[nodiscard]] Sample* data() noexcept {
        return data_;
    }

private:
    Sample* data_ = nullptr;
    std::size_t count_ = 0U;
};

[[nodiscard]] std::uint16_t bilinear_sample_channel(
    const std::span<const std::uint16_t> source,
    const Dimensions dimensions,
    const float source_x,
    const float source_y,
    const std::size_t channel
) noexcept {
    const auto width = static_cast<std::size_t>(dimensions.width);
    const auto height = static_cast<std::size_t>(dimensions.height);
    if (
        !std::isfinite(source_x) || !std::isfinite(source_y) || source_x < 0.0F
        || source_y < 0.0F || source_x > static_cast<float>(dimensions.width - 1U)
        || source_y > static_cast<float>(dimensions.height - 1U)
    ) {
        return 0U;
    }
    const auto x0 = static_cast<std::size_t>(std::floor(source_x));
    const auto y0 = static_cast<std::size_t>(std::floor(source_y));
    const auto x1 = std::min(x0 + 1U, width - 1U);
    const auto y1 = std::min(y0 + 1U, height - 1U);
    const auto horizontal = static_cast<double>(source_x) - static_cast<double>(x0);
    const auto vertical = static_cast<double>(source_y) - static_cast<double>(y0);
    const auto sample = [&](const std::size_t x, const std::size_t y) {
        return static_cast<double>(source[(y * width + x) * rgb_channels + channel]);
    };
    const auto upper = sample(x0, y0) + (sample(x1, y0) - sample(x0, y0)) * horizontal;
    const auto lower = sample(x0, y1) + (sample(x1, y1) - sample(x0, y1)) * horizontal;
    const auto value = upper + (lower - upper) * vertical;
    return static_cast<std::uint16_t>(std::clamp(
        std::llround(value),
        0LL,
        static_cast<long long>(std::numeric_limits<std::uint16_t>::max())
    ));
}

struct LensfunMatch final {
    const lfCamera* camera = nullptr;
    const lfLens* lens = nullptr;
    std::string camera_name;
    std::string lens_name;
};

struct LensfunResolution final {
    OpticsProfileStatus status = OpticsProfileStatus::camera_not_found;
    std::optional<LensfunMatch> match;
};

[[nodiscard]] std::string profile_match_key(
    const AssetMetadata& metadata,
    const OpticsSettings& settings
) {
    // These fields determine profile discovery only. Focal/aperture/distance are deliberately
    // absent: Lensfun interpolates calibration in the per-render modifier, not the match cache.
    return trim_ascii(metadata.make) + '\x1f' + trim_ascii(metadata.model) + '\x1f'
        + trim_ascii(metadata.normalized_make) + '\x1f' + trim_ascii(metadata.normalized_model)
        + '\x1f' + trim_ascii(metadata.lens_make) + '\x1f' + trim_ascii(metadata.lens_model)
        + '\x1f' + trim_ascii(settings.camera_profile_maker) + '\x1f'
        + trim_ascii(settings.camera_profile_model) + '\x1f'
        + trim_ascii(settings.lens_profile_maker) + '\x1f'
        + trim_ascii(settings.lens_profile_model);
}

[[nodiscard]] std::string lensfun_name(const lfMLstr value) {
    return value == nullptr ? std::string{} : std::string(lf_mlstr_get(value));
}

[[nodiscard]] float crop_factor(const AssetMetadata& metadata, const lfCamera& camera) noexcept {
    if (
        finite_positive(metadata.focal_length_mm) && finite_positive(metadata.focal_length_35mm)
    ) {
        const auto derived = metadata.focal_length_35mm / metadata.focal_length_mm;
        if (std::isfinite(derived) && derived >= 0.5 && derived <= 8.0) {
            return static_cast<float>(derived);
        }
    }
    return camera.CropFactor > 0.0F ? camera.CropFactor : 1.0F;
}

class LensfunOpticsProvider final : public OpticsProvider {
public:
    explicit LensfunOpticsProvider(std::optional<std::filesystem::path> database_directory) {
        info_.id = "lensfun";
        database_ = std::make_unique<lfDatabase>();
        bool loaded = false;
        if (database_directory.has_value()) {
#if LF_VERSION_MICRO >= 99
            loaded = database_->Load(database_directory->string().c_str()) == LF_NO_ERROR;
#else
            // Stable Lensfun 0.3.4's Load(path) reads one XML file; directory loading is a
            // separate API. The 0.3.99 development line folds directory discovery into Load().
            loaded = database_->LoadDirectory(database_directory->string().c_str());
#endif
        } else {
            loaded = database_->Load() == LF_NO_ERROR;
        }
        if (!loaded) {
            database_.reset();
            info_.version = "unavailable";
            return;
        }
        const auto directory = database_directory.has_value()
            ? database_directory->string()
            : std::string{"system"};
        info_.version = "lensfun-" + std::to_string(LF_VERSION_MAJOR) + "."
            + std::to_string(LF_VERSION_MINOR) + "." + std::to_string(LF_VERSION_MICRO)
            + ";database=" + directory;
        info_.available = true;
    }

    [[nodiscard]] const OpticsProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] std::vector<OpticsProfileCandidate> profile_candidates(
        const AssetMetadata& metadata
    ) const override {
        if (!info_.available || database_ == nullptr) {
            return {};
        }
        std::lock_guard lock(database_mutex_);
        const auto find_camera = [&](const std::string& make, const std::string& model)
            -> const lfCamera* {
            if (make.empty() || model.empty()) return nullptr;
            const auto* matches = database_->FindCameras(make.c_str(), model.c_str());
            if (matches == nullptr || matches[0] == nullptr) {
                if (matches != nullptr) lf_free(const_cast<lfCamera**>(matches));
                matches = database_->FindCamerasExt(
                    make.c_str(), model.c_str(), LF_SEARCH_LOOSE
                );
            }
            const auto* match = matches != nullptr ? matches[0] : nullptr;
            if (matches != nullptr) lf_free(const_cast<lfCamera**>(matches));
            return match;
        };
        const lfCamera* camera = find_camera(
            trim_ascii(metadata.make), trim_ascii(metadata.model)
        );
        if (camera == nullptr) {
            camera = find_camera(
                trim_ascii(metadata.normalized_make), trim_ascii(metadata.normalized_model)
            );
        }
        if (camera == nullptr) return {};

        // FindLenses(camera, maker, model) parses a human-readable model string and does not
        // treat a null model as "all compatible lenses". Build the structural query used by
        // Lensfun's second overload instead: the camera crop factor is the upper calibration
        // bound and its mount limits the result to profiles that can actually be mounted.
        lfLens compatible_lens;
        compatible_lens.CropFactor = camera->CropFactor;
        if (camera->Mount != nullptr && camera->Mount[0] != '\0') {
            compatible_lens.AddMount(camera->Mount);
        }
        const auto* lenses = database_->FindLenses(&compatible_lens);
        if (lenses == nullptr) return {};
        std::vector<OpticsProfileCandidate> result;
        constexpr std::size_t maximum_candidates = 2'048U;
        for (std::size_t index = 0U;
             lenses[index] != nullptr && result.size() < maximum_candidates;
             ++index) {
            const auto* lens = lenses[index];
            const auto camera_maker = lensfun_name(camera->Maker);
            const auto camera_model = lensfun_name(camera->Model);
            const auto lens_maker = lensfun_name(lens->Maker);
            const auto lens_model = lensfun_name(lens->Model);
            if (camera_model.empty() || lens_model.empty()) continue;
            result.push_back({camera_maker, camera_model, lens_maker, lens_model});
        }
        lf_free(const_cast<lfLens**>(lenses));
        std::ranges::sort(result, [](const auto& left, const auto& right) {
            return std::tie(left.lens_maker, left.lens_model)
                < std::tie(right.lens_maker, right.lens_model);
        });
        result.erase(
            std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
                return left.camera_maker == right.camera_maker
                    && left.camera_model == right.camera_model
                    && left.lens_maker == right.lens_maker
                    && left.lens_model == right.lens_model;
            }),
            result.end()
        );
        return result;
    }

    [[nodiscard]] OpticsCorrectionResult correct_reference_rgb(
        const PixelBuffer& input,
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const override {
        validate_settings(settings);
        if (!settings.enabled) {
            return OpticsCorrectionResult{.receipt = unavailable_receipt(
                info_,
                OpticsProfileStatus::disabled
            )};
        }
        if (!info_.available || database_ == nullptr) {
            return OpticsCorrectionResult{.receipt = unavailable_receipt(
                info_,
                OpticsProfileStatus::provider_unavailable
            )};
        }
        const bool manual_profile = !trim_ascii(settings.camera_profile_model).empty();
        if (
            !finite_positive(metadata.focal_length_mm)
            || (!manual_profile && (
                trim_ascii(metadata.lens_model).empty()
                || (trim_ascii(metadata.make).empty()
                    && trim_ascii(metadata.normalized_make).empty())
                || (trim_ascii(metadata.model).empty()
                    && trim_ascii(metadata.normalized_model).empty())
            ))
        ) {
            return OpticsCorrectionResult{.receipt = unavailable_receipt(
                info_,
                OpticsProfileStatus::insufficient_metadata
            )};
        }
        if (
            input.bits_per_channel != 16U || input.channels != rgb_channels
            || input.transfer_function != RgbTransferFunction::linear
            || input.primaries != RgbPrimaries::srgb_rec709_d65
            || input.reference != RgbBufferReference::processed_raw
        ) {
            return OpticsCorrectionResult{.receipt = unavailable_receipt(
                info_,
                OpticsProfileStatus::incompatible_input
            )};
        }
        validate_input(input);

        const auto resolution = resolve(metadata, settings);
        if (!resolution.match.has_value()) {
            return OpticsCorrectionResult{.receipt = unavailable_receipt(info_, resolution.status)};
        }
        const auto& match = *resolution.match;

        const auto width = input.dimensions.width;
        const auto height = input.dimensions.height;
        if (
            width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            || height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        ) {
            throw DecodeError(
                DecodeErrorCode::resource_limit,
                0,
                "optics image dimensions exceed Lensfun's integer API"
            );
        }

        // LibRaw does not expose a broadly reliable focus-distance field. Lightroom-like
        // workflows nevertheless apply the ordinary lens profile in this situation: distance
        // matters chiefly for close-focus corrections, while the far-distance calibration is
        // normally the useful default. Use an explicit 1 km approximation and report it in the
        // receipt instead of turning vignetting off for almost every LibRaw asset.
        const bool has_vignetting_aperture = finite_positive(metadata.aperture_f_number);
        const bool has_vignetting_distance = finite_positive(metadata.focus_distance_meters);
        const bool request_vignetting = settings.correct_vignetting && has_vignetting_aperture;
        const float vignetting_distance_meters = has_vignetting_distance
            ? static_cast<float>(metadata.focus_distance_meters)
            : 1000.0F;
        const bool vignetting_uses_distance_fallback = request_vignetting
            && !has_vignetting_distance;

#if LF_VERSION_MICRO >= 99
        // Lensfun 0.3.99+ replaced Initialize() with focused enable calls. Keep the adapter on
        // the native API of each supported line instead of emulating one through deprecated C
        // wrappers; Homebrew currently ships stable 0.3.4 while upstream development uses 0.3.99.
        lfModifier modifier(
            match.lens,
            static_cast<float>(metadata.focal_length_mm),
            crop_factor(metadata, *match.camera),
            static_cast<int>(width),
            static_cast<int>(height),
            LF_PF_U16
        );
        if (settings.correct_distortion) {
            modifier.EnableDistortionCorrection();
        }
        if (settings.correct_tca) {
            modifier.EnableTCACorrection();
        }
        if (request_vignetting) {
            modifier.EnableVignettingCorrection(
                static_cast<float>(metadata.aperture_f_number),
                vignetting_distance_meters
            );
        }

        auto flags = modifier.GetModFlags();
        const auto geometry_requested = (flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) != 0;
        bool applied_scaling = false;
        if (settings.automatic_scale && geometry_requested) {
            const auto automatic_scale = modifier.GetAutoScale(false);
            if (std::isfinite(automatic_scale) && automatic_scale > 0.0F) {
                modifier.EnableScaling(automatic_scale);
                flags = modifier.GetModFlags();
                applied_scaling = (flags & LF_MODIFY_SCALE) != 0;
            }
        }
#else
        // Lensfun 0.3.4 configures all requested corrections in one Initialize() call. A scale
        // of zero asks Lensfun to compute its calibrated automatic scale. At normal focus
        // distances, 1000 m is our documented fallback for a missing portable focus field;
        // when vignetting is not requested it remains an inert API placeholder.
        int requested_flags = 0;
        if (settings.correct_distortion) {
            requested_flags |= LF_MODIFY_DISTORTION;
        }
        if (settings.correct_tca) {
            requested_flags |= LF_MODIFY_TCA;
        }
        if (request_vignetting) {
            requested_flags |= LF_MODIFY_VIGNETTING;
        }
        const auto geometry_requested =
            (requested_flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) != 0;
        if (settings.automatic_scale && geometry_requested) {
            requested_flags |= LF_MODIFY_SCALE;
        }

        lfModifier modifier(
            match.lens,
            crop_factor(metadata, *match.camera),
            static_cast<int>(width),
            static_cast<int>(height)
        );
        const auto flags = modifier.Initialize(
            match.lens,
            LF_PF_U16,
            static_cast<float>(metadata.focal_length_mm),
            request_vignetting ? static_cast<float>(metadata.aperture_f_number) : 0.0F,
            request_vignetting ? vignetting_distance_meters : 1000.0F,
            settings.automatic_scale && geometry_requested ? 0.0F : 1.0F,
            match.lens->Type,
            requested_flags,
            false
        );
        const bool applied_scaling = (flags & LF_MODIFY_SCALE) != 0;
#endif

        OpticsProfileReceipt receipt{
            .status = OpticsProfileStatus::matched,
            .provider_id = info_.id,
            .provider_version = info_.version,
            .camera_profile = match.camera_name,
            .lens_profile = match.lens_name,
            .distortion_available = (flags & LF_MODIFY_DISTORTION) != 0,
            .tca_available = (flags & LF_MODIFY_TCA) != 0,
            .vignetting_available = (flags & LF_MODIFY_VIGNETTING) != 0,
            .applied_distortion = (flags & LF_MODIFY_DISTORTION) != 0,
            .applied_tca = (flags & LF_MODIFY_TCA) != 0,
            .applied_vignetting = (flags & LF_MODIFY_VIGNETTING) != 0,
            .vignetting_used_distance_fallback = vignetting_uses_distance_fallback
                && (flags & LF_MODIFY_VIGNETTING) != 0,
            .applied_scaling = applied_scaling,
        };
        if (
            !receipt.applied_distortion && !receipt.applied_tca
            && !receipt.applied_vignetting
        ) {
            return OpticsCorrectionResult{.receipt = std::move(receipt)};
        }

        AlignedSamples<std::uint16_t> color_corrected(input.samples.size());
        std::copy(input.samples.begin(), input.samples.end(), color_corrected.data());
        if (receipt.applied_vignetting) {
            const auto modified = modifier.ApplyColorModification(
                color_corrected.data(),
                0.0F,
                0.0F,
                static_cast<int>(width),
                static_cast<int>(height),
                LF_CR_3(RED, GREEN, BLUE),
                static_cast<int>(input.row_stride_bytes)
            );
            if (!modified) {
                receipt.applied_vignetting = false;
                receipt.vignetting_used_distance_fallback = false;
            }
        }

        PixelBuffer output = input;
        if (receipt.applied_distortion || receipt.applied_tca) {
            const auto pixel_count = static_cast<std::size_t>(width) * height;
            const auto batch_rows = std::min(remap_rows_per_batch, height);
            std::vector<float> coordinates(
                static_cast<std::size_t>(width) * batch_rows * rgb_channels * 2U
            );
            for (std::uint32_t row = 0U; row < height; row += batch_rows) {
                const auto rows = std::min(batch_rows, height - row);
                const auto remapped = modifier.ApplySubpixelGeometryDistortion(
                    0.0F,
                    static_cast<float>(row),
                    static_cast<int>(width),
                    static_cast<int>(rows),
                    coordinates.data()
                );
                if (!remapped) {
                    throw DecodeError(
                        DecodeErrorCode::internal,
                        0,
                        "Lensfun unexpectedly declined an enabled optical remap"
                    );
                }
                for (std::uint32_t local_y = 0U; local_y < rows; ++local_y) {
                    const auto output_y = row + local_y;
                    for (std::uint32_t x = 0U; x < width; ++x) {
                        const auto pixel = static_cast<std::size_t>(local_y) * width + x;
                        for (std::size_t channel = 0U; channel < rgb_channels; ++channel) {
                            const auto coordinate = (pixel * rgb_channels + channel) * 2U;
                            output.samples[(static_cast<std::size_t>(output_y) * width + x)
                                * rgb_channels + channel] = bilinear_sample_channel(
                                std::span<const std::uint16_t>(
                                    color_corrected.data(),
                                    pixel_count * rgb_channels
                                ),
                                input.dimensions,
                                coordinates[coordinate],
                                coordinates[coordinate + 1U],
                                channel
                            );
                        }
                    }
                }
            }
        } else if (receipt.applied_vignetting) {
            std::copy(
                color_corrected.data(),
                color_corrected.data() + static_cast<std::ptrdiff_t>(input.samples.size()),
                output.samples.begin()
            );
        }
        return OpticsCorrectionResult{
            .receipt = std::move(receipt),
            .corrected_reference_rgb = std::move(output),
        };
    }

private:
    [[nodiscard]] LensfunResolution resolve(
        const AssetMetadata& metadata,
        const OpticsSettings& settings
    ) const {
        std::lock_guard lock(database_mutex_);
        const auto cache_key = profile_match_key(metadata, settings);
        if (const auto cached = match_cache_.find(cache_key); cached != match_cache_.end()) {
            return cached->second;
        }
        const auto remember = [&](LensfunResolution result) {
            match_cache_.insert_or_assign(cache_key, result);
            return result;
        };
        const auto find_camera = [&](const std::string& make, const std::string& model)
            -> const lfCamera* {
            if (make.empty() || model.empty()) {
                return nullptr;
            }
            const auto* matches = database_->FindCameras(make.c_str(), model.c_str());
            if (matches == nullptr || matches[0] == nullptr) {
                if (matches != nullptr) {
                    lf_free(const_cast<lfCamera**>(matches));
                }
                matches = database_->FindCamerasExt(
                    make.c_str(), model.c_str(), LF_SEARCH_LOOSE
                );
            }
            if (matches == nullptr || matches[0] == nullptr) {
                if (matches != nullptr) {
                    lf_free(const_cast<lfCamera**>(matches));
                }
                return nullptr;
            }
            const auto* match = matches[0];
            lf_free(const_cast<lfCamera**>(matches));
            return match;
        };
        const bool manual = !trim_ascii(settings.camera_profile_model).empty();
        const auto camera = [&] {
            if (manual) {
                return find_camera(
                    trim_ascii(settings.camera_profile_maker),
                    trim_ascii(settings.camera_profile_model)
                );
            }
            if (const auto* exact = find_camera(trim_ascii(metadata.make), trim_ascii(metadata.model))) {
                return exact;
            }
            return find_camera(trim_ascii(metadata.normalized_make), trim_ascii(metadata.normalized_model));
        }();
        if (camera == nullptr) {
            return remember({.status = OpticsProfileStatus::camera_not_found});
        }
        const auto lens_make = trim_ascii(
            manual ? settings.lens_profile_maker : metadata.lens_make
        );
        const auto lens_model = trim_ascii(
            manual ? settings.lens_profile_model : metadata.lens_model
        );
        const auto* lenses = database_->FindLenses(
            camera,
            lens_make.empty() ? nullptr : lens_make.c_str(),
            lens_model.c_str()
        );
        if (lenses == nullptr || lenses[0] == nullptr) {
            if (lenses != nullptr) {
                lf_free(const_cast<lfLens**>(lenses));
            }
            return remember({.status = OpticsProfileStatus::lens_not_found});
        }
        const auto* lens = lenses[0];
        lf_free(const_cast<lfLens**>(lenses));
        return remember(LensfunResolution{
            .status = OpticsProfileStatus::matched,
            .match = LensfunMatch{
                .camera = camera,
                .lens = lens,
                .camera_name = lensfun_name(camera->Model),
                .lens_name = lensfun_name(lens->Model),
            },
        });
    }

    OpticsProviderInfo info_;
    std::unique_ptr<lfDatabase> database_;
    mutable std::mutex database_mutex_;
    mutable std::unordered_map<std::string, LensfunResolution> match_cache_;
};

#else

class LensfunOpticsProvider final : public OpticsProvider {
public:
    explicit LensfunOpticsProvider(const std::optional<std::filesystem::path>&) {
        info_.id = "lensfun";
        info_.version = "not-linked";
    }

    [[nodiscard]] const OpticsProviderInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] OpticsCorrectionResult correct_reference_rgb(
        const PixelBuffer&,
        const AssetMetadata&,
        const OpticsSettings& settings
    ) const override {
        validate_settings(settings);
        return OpticsCorrectionResult{.receipt = unavailable_receipt(
            info_,
            settings.enabled ? OpticsProfileStatus::provider_unavailable
                             : OpticsProfileStatus::disabled
        )};
    }

    [[nodiscard]] std::vector<OpticsProfileCandidate> profile_candidates(
        const AssetMetadata&
    ) const override {
        return {};
    }

private:
    OpticsProviderInfo info_;
};

#endif

} // namespace

OpticsSettings default_optics_settings() noexcept {
    return {};
}

std::string optics_settings_signature(const OpticsSettings& settings) {
    validate_settings(settings);
    std::ostringstream signature;
    signature << "shadow-optics-v" << optics_implementation_version
              << ";enabled=" << (settings.enabled ? 1 : 0)
              << ";distortion=" << (settings.correct_distortion ? 1 : 0)
              << ";tca=" << (settings.correct_tca ? 1 : 0)
              << ";vignetting=" << (settings.correct_vignetting ? 1 : 0)
              << ";auto-scale=" << (settings.automatic_scale ? 1 : 0);
    signature << ";camera-maker=" << settings.camera_profile_maker
              << ";camera-model=" << settings.camera_profile_model
              << ";lens-maker=" << settings.lens_profile_maker
              << ";lens-model=" << settings.lens_profile_model;
    return signature.str();
}

std::shared_ptr<const OpticsProvider> make_lensfun_optics_provider(
    std::optional<std::filesystem::path> database_directory
) {
    return std::make_shared<LensfunOpticsProvider>(std::move(database_directory));
}

} // namespace shadow::image

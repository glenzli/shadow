#include "neural_raw_denoise.hpp"

#include "coreml_neural_raw_denoise.hpp"

#include "../../concurrency/row_scheduler.hpp"

#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shadow::image::detail {

namespace {

inline constexpr std::uint32_t maximum_packed_tile_edge = 1024U;

struct CanonicalBayerSites final {
    // Raw-coordinate parity site indices in canonical R, Gr, Gb, B order.
    std::array<std::size_t, 4U> sites{};
    // Offsets from a packed cell anchored at the active rectangle's top-left.
    std::array<std::uint32_t, 4U> offset_x{};
    std::array<std::uint32_t, 4U> offset_y{};
};

[[nodiscard]] bool is_lower_hex(const std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](const char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    });
}

[[nodiscard]] bool valid_model_content_identity(const std::string_view identity) noexcept {
    constexpr std::string_view prefix = "sha256-tree-v1:";
    return identity.size() == prefix.size() + 64U && identity.starts_with(prefix)
           && is_lower_hex(identity.substr(prefix.size()));
}

[[nodiscard]] bool valid_tile_contract(
    const std::uint32_t tile_edge,
    const std::uint32_t halo
) noexcept {
    return tile_edge >= 4U && tile_edge <= maximum_packed_tile_edge
           && halo < tile_edge / 2U;
}

[[nodiscard]] std::optional<std::uint32_t> parse_u32_environment(const char* value) noexcept {
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0'
        || parsed > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] bool environment_is_one(const char* value) noexcept {
    return value != nullptr && std::string_view(value) == "1";
}

[[nodiscard]] CanonicalBayerSites canonical_bayer_sites(
    const RawFrameDescriptor& descriptor
) {
    std::size_t red = 4U;
    std::size_t blue = 4U;
    std::array<std::size_t, 2U> greens{};
    std::size_t green_count = 0U;
    for (std::size_t site = 0U; site < descriptor.bayer_2x2.size(); ++site) {
        switch (descriptor.bayer_2x2[site]) {
        case RawCfaColor::red:
            red = site;
            break;
        case RawCfaColor::green:
            if (green_count < greens.size()) {
                greens[green_count++] = site;
            }
            break;
        case RawCfaColor::blue:
            blue = site;
            break;
        case RawCfaColor::unknown:
            break;
        }
    }
    if (red >= 4U || blue >= 4U || green_count != 2U) {
        throw DecodeError(
            DecodeErrorCode::unsupported_layout,
            0,
            "neural RAW denoise requires one red, two green, and one blue Bayer site"
        );
    }
    const std::size_t red_row = red / 2U;
    const std::size_t gr = greens[0U] / 2U == red_row ? greens[0U] : greens[1U];
    const std::size_t gb = gr == greens[0U] ? greens[1U] : greens[0U];

    CanonicalBayerSites result;
    result.sites = {red, gr, gb, blue};
    const std::uint32_t active_x_parity = descriptor.active_margins.left & 1U;
    const std::uint32_t active_y_parity = descriptor.active_margins.top & 1U;
    for (std::size_t channel = 0U; channel < result.sites.size(); ++channel) {
        const auto site = static_cast<std::uint32_t>(result.sites[channel]);
        result.offset_x[channel] = (site & 1U) ^ active_x_parity;
        result.offset_y[channel] = ((site / 2U) & 1U) ^ active_y_parity;
    }
    return result;
}

[[nodiscard]] std::uint32_t last_coordinate_with_parity(
    const std::uint32_t extent,
    const std::uint32_t parity
) noexcept {
    std::uint32_t coordinate = extent - 1U;
    if ((coordinate & 1U) != parity) {
        --coordinate;
    }
    return coordinate;
}

[[nodiscard]] std::uint32_t active_coordinate(
    const std::uint32_t packed_coordinate,
    const std::uint32_t offset,
    const std::uint32_t extent
) noexcept {
    const std::uint64_t requested =
        static_cast<std::uint64_t>(packed_coordinate) * 2U + offset;
    if (requested < extent) {
        return static_cast<std::uint32_t>(requested);
    }
    return last_coordinate_with_parity(extent, offset);
}

[[nodiscard]] float normalized_sample(
    const RawFrame& frame,
    const CanonicalBayerSites& bayer,
    const std::size_t channel,
    const std::uint32_t packed_x,
    const std::uint32_t packed_y
) noexcept {
    const auto& descriptor = frame.descriptor;
    const std::uint32_t relative_x = active_coordinate(
        packed_x,
        bayer.offset_x[channel],
        descriptor.active_dimensions.width
    );
    const std::uint32_t relative_y = active_coordinate(
        packed_y,
        bayer.offset_y[channel],
        descriptor.active_dimensions.height
    );
    const std::uint32_t raw_x = descriptor.active_margins.left + relative_x;
    const std::uint32_t raw_y = descriptor.active_margins.top + relative_y;
    const std::size_t raw_index =
        static_cast<std::size_t>(raw_y) * descriptor.storage_dimensions.width + raw_x;
    const std::size_t site = bayer.sites[channel];
    const double black = descriptor.black_levels[site];
    const double range = static_cast<double>(descriptor.white_levels[site]) - black;
    const double normalized = (static_cast<double>(frame.samples[raw_index]) - black) / range;
    return static_cast<float>(std::clamp(normalized, 0.0, 1.0));
}

[[nodiscard]] std::array<float, 8U> normalized_noise(
    const RawFrameDescriptor& descriptor,
    const CanonicalBayerSites& bayer
) noexcept {
    std::array<float, 8U> noise{};
    for (std::size_t channel = 0U; channel < bayer.sites.size(); ++channel) {
        const std::size_t site = bayer.sites[channel];
        const double range = static_cast<double>(descriptor.white_levels[site])
                             - static_cast<double>(descriptor.black_levels[site]);
        noise[channel] =
            static_cast<float>(descriptor.sensor_noise.read_noise_stddev_dn[site] / range);
        // If z=(DN-black)/range, then Var(z)=shot/range*z + (read/range)^2.
        noise[channel + 4U] = static_cast<float>(
            descriptor.sensor_noise.shot_noise_variance_per_dn[site] / range
        );
    }
    return noise;
}

void store_normalized_sample(
    RawFrame& output,
    const CanonicalBayerSites& bayer,
    const std::size_t channel,
    const std::uint32_t packed_x,
    const std::uint32_t packed_y,
    const float normalized
) noexcept {
    const auto& descriptor = output.descriptor;
    const std::uint64_t relative_x =
        static_cast<std::uint64_t>(packed_x) * 2U + bayer.offset_x[channel];
    const std::uint64_t relative_y =
        static_cast<std::uint64_t>(packed_y) * 2U + bayer.offset_y[channel];
    if (relative_x >= descriptor.active_dimensions.width
        || relative_y >= descriptor.active_dimensions.height) {
        return;
    }
    const std::uint32_t raw_x =
        descriptor.active_margins.left + static_cast<std::uint32_t>(relative_x);
    const std::uint32_t raw_y =
        descriptor.active_margins.top + static_cast<std::uint32_t>(relative_y);
    const std::size_t raw_index =
        static_cast<std::size_t>(raw_y) * descriptor.storage_dimensions.width + raw_x;
    const std::size_t site = bayer.sites[channel];
    const double black = descriptor.black_levels[site];
    const double range = static_cast<double>(descriptor.white_levels[site]) - black;
    const double denormalized =
        black + static_cast<double>(std::clamp(normalized, 0.0F, 1.0F)) * range;
    output.samples[raw_index] = static_cast<std::uint16_t>(std::clamp(
        std::lround(denormalized),
        0L,
        static_cast<long>(std::numeric_limits<std::uint16_t>::max())
    ));
}

[[nodiscard]] NeuralRawDenoiseStatus
status_for_readiness(const NeuralRawDenoiseReadiness readiness) noexcept {
    switch (readiness) {
    case NeuralRawDenoiseReadiness::disabled:
        return NeuralRawDenoiseStatus::disabled;
    case NeuralRawDenoiseReadiness::bypassed_preview:
        return NeuralRawDenoiseStatus::bypassed_preview;
    case NeuralRawDenoiseReadiness::unsupported_source:
        return NeuralRawDenoiseStatus::fallback_unsupported_source;
    case NeuralRawDenoiseReadiness::invalid_configuration:
        return NeuralRawDenoiseStatus::fallback_invalid_configuration;
    case NeuralRawDenoiseReadiness::ready:
        break;
    }
    return NeuralRawDenoiseStatus::fallback_runtime_failure;
}

[[nodiscard]] std::string cache_identity(
    const PreparedNeuralRawDenoise& prepared,
    const NeuralRawDenoiseStatus status,
    const NeuralRawDenoiseBackend backend,
    const std::string_view model_content_identity,
    const std::string_view backend_execution_identity
) {
    std::ostringstream identity;
    identity << "neural-raw-denoise=" << neural_raw_denoise_status_identity(status)
             << ";neural-raw-denoise-backend=" << neural_raw_denoise_backend_identity(backend)
             << ";neural-raw-denoise-contract=" << neural_raw_denoise_preprocessing_contract;
    if (prepared.requested()) {
        identity << ";neural-raw-denoise-model="
                 << (model_content_identity.empty() ? "unverified" : model_content_identity)
                 << ";neural-raw-denoise-tile=" << prepared.configuration.packed_tile_edge
                 << ";neural-raw-denoise-halo=" << prepared.configuration.packed_halo;
    } else {
        identity << ";neural-raw-denoise-model=none";
    }
    identity << ";neural-raw-denoise-runtime="
             << (backend_execution_identity.empty() ? "none" : backend_execution_identity);
    return identity.str();
}

[[nodiscard]] NeuralRawDenoiseReceipt make_receipt(
    const PreparedNeuralRawDenoise& prepared,
    const NeuralRawDenoiseStatus status,
    const NeuralRawDenoiseBackend backend,
    std::string model_content_identity,
    std::string backend_execution_identity,
    std::string diagnostic
) {
    NeuralRawDenoiseReceipt receipt{
        .schema_version = neural_raw_denoise_receipt_schema_version,
        .status = status,
        .backend = backend,
        .model_content_identity = std::move(model_content_identity),
        .backend_execution_identity = std::move(backend_execution_identity),
        .diagnostic = std::move(diagnostic),
    };
    receipt.cache_identity = cache_identity(
        prepared,
        status,
        backend,
        receipt.model_content_identity,
        receipt.backend_execution_identity
    );
    return receipt;
}

} // namespace

const char* neural_raw_denoise_status_identity(const NeuralRawDenoiseStatus status) noexcept {
    switch (status) {
    case NeuralRawDenoiseStatus::disabled:
        return "disabled";
    case NeuralRawDenoiseStatus::bypassed_preview:
        return "bypassed-preview";
    case NeuralRawDenoiseStatus::fallback_unsupported_source:
        return "fallback-unsupported-source";
    case NeuralRawDenoiseStatus::fallback_invalid_configuration:
        return "fallback-invalid-configuration";
    case NeuralRawDenoiseStatus::fallback_runtime_failure:
        return "fallback-runtime-failure";
    case NeuralRawDenoiseStatus::applied:
        return "applied";
    }
    return "unknown";
}

const char* neural_raw_denoise_backend_identity(const NeuralRawDenoiseBackend backend) noexcept {
    switch (backend) {
    case NeuralRawDenoiseBackend::none:
        return "none";
    case NeuralRawDenoiseBackend::core_ml:
        return "core-ml";
    }
    return "unknown";
}

bool NeuralRawDenoiseReceipt::applied() const noexcept {
    return status == NeuralRawDenoiseStatus::applied;
}

bool NeuralRawDenoiseReceipt::valid() const noexcept {
    const std::string_view cache(cache_identity);
    const auto has_field = [cache](
                               const std::string_view key,
                               const std::string_view value
                           ) noexcept {
        const std::size_t key_position = cache.find(key);
        if (key_position == std::string_view::npos) {
            return false;
        }
        const std::size_t value_position = key_position + key.size();
        return cache.substr(value_position, value.size()) == value
               && (value_position + value.size() == cache.size()
                   || cache[value_position + value.size()] == ';');
    };
    if (schema_version != neural_raw_denoise_receipt_schema_version
        || !has_field("neural-raw-denoise=", neural_raw_denoise_status_identity(status))
        || !has_field(
            "neural-raw-denoise-backend=",
            neural_raw_denoise_backend_identity(backend)
        )) {
        return false;
    }
    if (applied()) {
        return backend == NeuralRawDenoiseBackend::core_ml
               && valid_model_content_identity(model_content_identity)
               && backend_execution_identity.starts_with("core-ml-v1:")
               && has_field("neural-raw-denoise-model=", model_content_identity)
               && has_field(
                   "neural-raw-denoise-runtime=",
                   backend_execution_identity
               )
               && diagnostic.empty();
    }
    return backend == NeuralRawDenoiseBackend::none && backend_execution_identity.empty()
           && has_field("neural-raw-denoise-runtime=", "none");
}

bool PreparedNeuralRawDenoise::requested() const noexcept {
    return readiness != NeuralRawDenoiseReadiness::disabled;
}

bool PreparedNeuralRawDenoise::execution_requested() const noexcept {
    return readiness == NeuralRawDenoiseReadiness::ready;
}

NeuralRawDenoiseConfiguration neural_raw_denoise_configuration_from_environment() {
    NeuralRawDenoiseConfiguration configuration;
    const char* path = std::getenv("SHADOW_AI_RAW_DENOISE_MODEL");
    const char* identity = std::getenv("SHADOW_AI_RAW_DENOISE_MODEL_IDENTITY");
    const char* tile = std::getenv("SHADOW_AI_RAW_DENOISE_TILE_EDGE");
    const char* halo = std::getenv("SHADOW_AI_RAW_DENOISE_HALO");
    const char* previews = std::getenv("SHADOW_AI_RAW_DENOISE_PREVIEWS");
    const bool any_value = path != nullptr || identity != nullptr || tile != nullptr
                           || halo != nullptr || previews != nullptr;
    if (!any_value) {
        return configuration;
    }
    configuration.enabled = true;
    configuration.compiled_model_path = path == nullptr ? "" : path;
    configuration.expected_model_content_identity = identity == nullptr ? "" : identity;
    configuration.apply_to_preview = environment_is_one(previews);

    if (tile != nullptr) {
        const auto parsed = parse_u32_environment(tile);
        if (!parsed.has_value()) {
            configuration.configuration_error =
                "SHADOW_AI_RAW_DENOISE_TILE_EDGE is not an unsigned integer";
            return configuration;
        }
        configuration.packed_tile_edge = *parsed;
    }
    if (halo != nullptr) {
        const auto parsed = parse_u32_environment(halo);
        if (!parsed.has_value()) {
            configuration.configuration_error =
                "SHADOW_AI_RAW_DENOISE_HALO is not an unsigned integer";
            return configuration;
        }
        configuration.packed_halo = *parsed;
    }
    if (previews != nullptr && std::string_view(previews) != "0"
        && std::string_view(previews) != "1") {
        configuration.configuration_error =
            "SHADOW_AI_RAW_DENOISE_PREVIEWS must be 0 or 1";
    }
    return configuration;
}

PreparedNeuralRawDenoise prepare_neural_raw_denoise(
    const RawFrame& frame,
    const NeuralRawDenoiseConfiguration& configuration,
    const bool preview
) {
    PreparedNeuralRawDenoise prepared;
    prepared.configuration = configuration;
    if (!configuration.enabled) {
        return prepared;
    }
    if (!configuration.configuration_error.empty()) {
        prepared.readiness = NeuralRawDenoiseReadiness::invalid_configuration;
        prepared.diagnostic = configuration.configuration_error;
        return prepared;
    }
    if (configuration.compiled_model_path.empty()
        || !valid_model_content_identity(configuration.expected_model_content_identity)
        || !valid_tile_contract(
            configuration.packed_tile_edge,
            configuration.packed_halo
        )) {
        prepared.readiness = NeuralRawDenoiseReadiness::invalid_configuration;
        prepared.diagnostic =
            "neural RAW denoise requires a compiled model path, a lowercase "
            "sha256-tree-v1 identity, and a bounded tile/halo contract";
        return prepared;
    }
    if (preview && !configuration.apply_to_preview) {
        prepared.readiness = NeuralRawDenoiseReadiness::bypassed_preview;
        prepared.diagnostic = "neural RAW denoise is bypassed for preview development";
        return prepared;
    }
    const auto& calibration = frame.descriptor.sensor_noise;
    if (!frame.valid() || !frame.is_bayer_2x2()
        || frame.descriptor.active_dimensions.width < 2U
        || frame.descriptor.active_dimensions.height < 2U
        || calibration.model != RawSensorNoiseModel::poisson_gaussian_per_cfa
        || calibration.source == RawSensorNoiseCalibrationSource::unavailable) {
        prepared.readiness = NeuralRawDenoiseReadiness::unsupported_source;
        prepared.diagnostic =
            "neural RAW denoise requires a valid Bayer frame and calibrated per-CFA "
            "Poisson-Gaussian sensor noise";
        return prepared;
    }
    prepared.readiness = NeuralRawDenoiseReadiness::ready;
    return prepared;
}

PreparedNeuralRawDenoise
prepare_neural_raw_denoise_from_environment(const RawFrame& frame, const bool preview) {
    return prepare_neural_raw_denoise(
        frame,
        neural_raw_denoise_configuration_from_environment(),
        preview
    );
}

RawFrame execute_neural_raw_denoise_tiles(
    const RawFrame& source,
    const std::uint32_t packed_tile_edge,
    const std::uint32_t packed_halo,
    NeuralRawTileInference& inference
) {
    if (!source.valid() || !source.is_bayer_2x2()
        || source.descriptor.active_dimensions.width < 2U
        || source.descriptor.active_dimensions.height < 2U
        || !valid_tile_contract(packed_tile_edge, packed_halo)) {
        throw DecodeError(
            DecodeErrorCode::invalid_request,
            0,
            "neural RAW tile execution received an invalid source or tile contract"
        );
    }
    const auto& descriptor = source.descriptor;
    const CanonicalBayerSites bayer = canonical_bayer_sites(descriptor);
    const std::uint32_t packed_width = (descriptor.active_dimensions.width + 1U) / 2U;
    const std::uint32_t packed_height = (descriptor.active_dimensions.height + 1U) / 2U;
    const std::uint32_t core_edge = packed_tile_edge - 2U * packed_halo;
    const std::size_t tile_pixels =
        static_cast<std::size_t>(packed_tile_edge) * packed_tile_edge;
    const std::size_t tensor_values = tile_pixels * 4U;
    std::vector<float> input(tensor_values);
    std::vector<float> output(tensor_values);
    const auto noise = normalized_noise(descriptor, bayer);
    RawFrame result = source;

    for (std::uint32_t core_y = 0U; core_y < packed_height; core_y += core_edge) {
        for (std::uint32_t core_x = 0U; core_x < packed_width; core_x += core_edge) {
            throw_if_row_cancelled();
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                float* channel_input = input.data() + channel * tile_pixels;
                for (std::uint32_t tile_y = 0U; tile_y < packed_tile_edge; ++tile_y) {
                    const std::int64_t requested_y =
                        static_cast<std::int64_t>(core_y) + tile_y - packed_halo;
                    const std::uint32_t packed_y = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                        requested_y,
                        0,
                        static_cast<std::int64_t>(packed_height) - 1
                    ));
                    for (std::uint32_t tile_x = 0U; tile_x < packed_tile_edge; ++tile_x) {
                        const std::int64_t requested_x =
                            static_cast<std::int64_t>(core_x) + tile_x - packed_halo;
                        const std::uint32_t packed_x =
                            static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                                requested_x,
                                0,
                                static_cast<std::int64_t>(packed_width) - 1
                            ));
                        channel_input[
                            static_cast<std::size_t>(tile_y) * packed_tile_edge + tile_x
                        ] = normalized_sample(source, bayer, channel, packed_x, packed_y);
                    }
                }
            }
            std::fill(
                output.begin(),
                output.end(),
                std::numeric_limits<float>::quiet_NaN()
            );
            inference.infer(input, noise, packed_tile_edge, output);
            throw_if_row_cancelled();
            if (std::any_of(output.begin(), output.end(), [](const float value) {
                    return !std::isfinite(value);
                })) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "neural RAW inference returned a non-finite tensor"
                );
            }

            const std::uint32_t copy_width = std::min(core_edge, packed_width - core_x);
            const std::uint32_t copy_height = std::min(core_edge, packed_height - core_y);
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                const float* channel_output = output.data() + channel * tile_pixels;
                for (std::uint32_t y = 0U; y < copy_height; ++y) {
                    const std::uint32_t tile_y = packed_halo + y;
                    for (std::uint32_t x = 0U; x < copy_width; ++x) {
                        const std::uint32_t tile_x = packed_halo + x;
                        store_normalized_sample(
                            result,
                            bayer,
                            channel,
                            core_x + x,
                            core_y + y,
                            channel_output[
                                static_cast<std::size_t>(tile_y) * packed_tile_edge + tile_x
                            ]
                        );
                    }
                }
            }
        }
    }
    return result;
}

NeuralRawDenoiseResult
execute_prepared_neural_raw_denoise(RawFrame frame, const PreparedNeuralRawDenoise& prepared) {
    if (!prepared.execution_requested()) {
        const NeuralRawDenoiseStatus status = status_for_readiness(prepared.readiness);
        const std::string model_identity =
            prepared.requested()
                    && valid_model_content_identity(
                        prepared.configuration.expected_model_content_identity
                    )
                ? prepared.configuration.expected_model_content_identity
                : "";
        return NeuralRawDenoiseResult{
            .frame = std::move(frame),
            .receipt = make_receipt(
                prepared,
                status,
                NeuralRawDenoiseBackend::none,
                model_identity,
                {},
                prepared.diagnostic
            ),
        };
    }

    CoreMlNeuralRawDenoiseAttempt attempt =
        try_execute_coreml_neural_raw_denoise(frame, prepared);
    if (!attempt.frame.has_value()) {
        return NeuralRawDenoiseResult{
            .frame = std::move(frame),
            .receipt = make_receipt(
                prepared,
                NeuralRawDenoiseStatus::fallback_runtime_failure,
                NeuralRawDenoiseBackend::none,
                prepared.configuration.expected_model_content_identity,
                {},
                attempt.diagnostic.empty() ? "Core ML neural RAW denoise failed"
                                           : std::move(attempt.diagnostic)
            ),
        };
    }
    NeuralRawDenoiseReceipt receipt = make_receipt(
        prepared,
        NeuralRawDenoiseStatus::applied,
        NeuralRawDenoiseBackend::core_ml,
        std::move(attempt.verified_model_content_identity),
        std::move(attempt.backend_execution_identity),
        {}
    );
    if (!receipt.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "neural RAW denoise produced invalid execution provenance"
        );
    }
    return NeuralRawDenoiseResult{
        .frame = std::move(*attempt.frame),
        .receipt = std::move(receipt),
    };
}

std::string combined_raw_denoise_cache_identity(
    const NeuralRawDenoiseReceipt& neural,
    const RawBayerDenoiseReceipt& conventional
) {
    if (!neural.valid() || !conventional.valid()) {
        throw DecodeError(
            DecodeErrorCode::internal,
            0,
            "RAW denoise pipeline cannot combine invalid stage provenance"
        );
    }
    return neural.cache_identity + ";" + conventional.cache_identity;
}

} // namespace shadow::image::detail

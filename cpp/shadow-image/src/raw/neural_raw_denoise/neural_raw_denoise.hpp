#pragma once

#include <shadow/image/raw_denoise.hpp>
#include <shadow/image/raw_frame.hpp>

#include <cstdint>
#include <span>
#include <string>

namespace shadow::image::detail {

inline constexpr std::uint32_t neural_raw_denoise_receipt_schema_version = 1U;
inline constexpr const char* neural_raw_denoise_preprocessing_contract =
    "packed-bayer-r-gr-gb-b-poisson-gaussian-v1";

enum class NeuralRawDenoiseStatus : std::uint8_t {
    disabled,
    bypassed_preview,
    fallback_unsupported_source,
    fallback_invalid_configuration,
    fallback_runtime_failure,
    applied,
};

enum class NeuralRawDenoiseBackend : std::uint8_t {
    none,
    core_ml,
};

struct NeuralRawDenoiseReceipt final {
    std::uint32_t schema_version = neural_raw_denoise_receipt_schema_version;
    NeuralRawDenoiseStatus status = NeuralRawDenoiseStatus::disabled;
    NeuralRawDenoiseBackend backend = NeuralRawDenoiseBackend::none;
    std::string model_content_identity;
    std::string backend_execution_identity;
    std::string cache_identity;
    // Diagnostics are deliberately excluded from cache identity. They describe a local failed
    // attempt and may contain platform text, while status and the requested artifact identity are
    // the durable facts.
    std::string diagnostic;

    [[nodiscard]] bool applied() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
};

struct NeuralRawDenoiseConfiguration final {
    bool enabled = false;
    bool apply_to_preview = false;
    std::string compiled_model_path;
    // A compiled Core ML model is a directory. Its identity is the deterministic digest
    // `sha256-tree-v1:<64 lowercase hex>` over relative paths and file bytes.
    std::string expected_model_content_identity;
    std::uint32_t packed_tile_edge = 512U;
    std::uint32_t packed_halo = 32U;
    std::string configuration_error;
};

enum class NeuralRawDenoiseReadiness : std::uint8_t {
    disabled,
    bypassed_preview,
    unsupported_source,
    invalid_configuration,
    ready,
};

struct PreparedNeuralRawDenoise final {
    NeuralRawDenoiseReadiness readiness = NeuralRawDenoiseReadiness::disabled;
    NeuralRawDenoiseConfiguration configuration;
    std::string diagnostic;

    [[nodiscard]] bool requested() const noexcept;
    [[nodiscard]] bool execution_requested() const noexcept;
};

struct NeuralRawDenoiseResult final {
    RawFrame frame;
    NeuralRawDenoiseReceipt receipt;
};

// Backend-neutral tile inference boundary. The mosaic is contiguous NCHW float32 with shape
// [1, 4, edge, edge] in canonical R/Gr/Gb/B order. Noise is [1, 8]: four normalized read-noise
// standard deviations followed by four normalized shot-variance slopes in the same order.
class NeuralRawTileInference {
  public:
    NeuralRawTileInference() = default;
    NeuralRawTileInference(const NeuralRawTileInference&) = delete;
    NeuralRawTileInference& operator=(const NeuralRawTileInference&) = delete;
    virtual ~NeuralRawTileInference() = default;

    virtual void infer(
        std::span<const float> packed_mosaic,
        std::span<const float> noise,
        std::uint32_t packed_tile_edge,
        std::span<float> denoised_packed_mosaic
    ) = 0;
};

[[nodiscard]] const char*
neural_raw_denoise_status_identity(NeuralRawDenoiseStatus status) noexcept;
[[nodiscard]] const char*
neural_raw_denoise_backend_identity(NeuralRawDenoiseBackend backend) noexcept;

[[nodiscard]] NeuralRawDenoiseConfiguration
neural_raw_denoise_configuration_from_environment();

[[nodiscard]] PreparedNeuralRawDenoise prepare_neural_raw_denoise(
    const RawFrame& frame,
    const NeuralRawDenoiseConfiguration& configuration,
    bool preview
);

[[nodiscard]] PreparedNeuralRawDenoise
prepare_neural_raw_denoise_from_environment(const RawFrame& frame, bool preview);

// Executes the complete backend-neutral packing, tiling, and unpacking transaction. `source` is
// borrowed and the returned frame is independently owned, so a backend exception cannot publish a
// partially denoised sensor plane.
[[nodiscard]] RawFrame execute_neural_raw_denoise_tiles(
    const RawFrame& source,
    std::uint32_t packed_tile_edge,
    std::uint32_t packed_halo,
    NeuralRawTileInference& inference
);

[[nodiscard]] NeuralRawDenoiseResult
execute_prepared_neural_raw_denoise(RawFrame frame, const PreparedNeuralRawDenoise& prepared);

[[nodiscard]] std::string combined_raw_denoise_cache_identity(
    const NeuralRawDenoiseReceipt& neural,
    const RawBayerDenoiseReceipt& conventional
);

} // namespace shadow::image::detail

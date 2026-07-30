#pragma once

#include "neural_raw_denoise.hpp"

#include <optional>
#include <string>

namespace shadow::image::detail {

struct CoreMlNeuralRawDenoiseAttempt final {
    std::optional<RawFrame> frame;
    std::string verified_model_content_identity;
    std::string backend_execution_identity;
    std::string diagnostic;
};

[[nodiscard]] CoreMlNeuralRawDenoiseAttempt try_execute_coreml_neural_raw_denoise(
    const RawFrame& source,
    const PreparedNeuralRawDenoise& prepared
);

} // namespace shadow::image::detail

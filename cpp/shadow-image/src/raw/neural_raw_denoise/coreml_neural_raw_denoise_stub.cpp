#include "coreml_neural_raw_denoise.hpp"

namespace shadow::image::detail {

CoreMlNeuralRawDenoiseAttempt try_execute_coreml_neural_raw_denoise(
    const RawFrame&,
    const PreparedNeuralRawDenoise&
) {
    return CoreMlNeuralRawDenoiseAttempt{
        .diagnostic = "Core ML neural RAW denoise is unavailable on this platform",
    };
}

} // namespace shadow::image::detail

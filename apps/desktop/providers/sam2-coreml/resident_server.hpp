#pragma once

#include <string>

namespace shadow_sam2_coreml {

class Engine;

// Runs the bounded JSON-lines protocol on stdin/stdout until an explicit
// shutdown request or EOF. Diagnostics remain on stderr.
[[nodiscard]] bool run_resident_server(
    Engine& engine,
    const std::string& model_revision
);

} // namespace shadow_sam2_coreml

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace shadow_sam2_coreml {

inline constexpr std::size_t kModelEdge = 1024;
inline constexpr std::size_t kMaskEdge = 256;
inline constexpr std::size_t kMaximumPromptPoints = 16;

struct PromptPoint final {
    double x = 0.0;
    double y = 0.0;
    bool foreground = false;
};

struct MaskReceipt final {
    double score = 0.0;
    std::size_t point_count = 0;
};

// Owns the compiled Core ML models and the embedding for at most one rendered
// image. The caller supplies a complete content identity so repeated prompt
// refinements can reuse the embedding without coupling the engine to Shadow's
// hashing implementation.
class Engine final {
  public:
    static std::unique_ptr<Engine> load(const std::string& model_directory);

    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] bool load_image(
        const std::string& input_path,
        const std::string& content_identity,
        bool& cache_hit
    );
    [[nodiscard]] bool has_loaded_image(const std::string& content_identity) const;
    [[nodiscard]] std::optional<MaskReceipt> predict(
        const std::vector<PromptPoint>& points,
        const std::string& output_path
    );

  private:
    struct Impl;

    explicit Engine(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace shadow_sam2_coreml

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace shadow::image {

// LittleCMS is deliberately kept behind this small value-oriented contract.  RAW providers may
// continue to expose a known linear working space, while DNG/ICC input profiles and export/display
// profiles use the same content-addressed transform path.  `id` is a stable content fingerprint,
// not a filesystem location, so changing an ICC file cannot silently reuse a rendered cache.
struct IccProfileInfo final {
    std::string id;
    std::string description;
    std::uint64_t serialized_bytes = 0;
};

enum class IccRenderingIntent : std::uint8_t {
    perceptual,
    relative_colorimetric,
    saturation,
    absolute_colorimetric,
};

class IccTransform;

class IccProfile final {
public:
    // Opaque ownership state; public only so standard shared_ptr helpers can remain in the
    // implementation unit. Its fields and construction stay private to LittleCMS adaptation.
    struct State;

    IccProfile();
    IccProfile(const IccProfile&) = default;
    IccProfile& operator=(const IccProfile&) = default;
    IccProfile(IccProfile&&) noexcept = default;
    IccProfile& operator=(IccProfile&&) noexcept = default;
    ~IccProfile();

    [[nodiscard]] const IccProfileInfo& info() const noexcept;

private:
    explicit IccProfile(std::shared_ptr<const State> state);

    std::shared_ptr<const State> state_;

    friend IccProfile make_linear_srgb_icc_profile();
    friend IccProfile make_display_srgb_icc_profile();
    friend IccProfile load_icc_profile(const std::filesystem::path& path);
    friend IccTransform make_icc_transform(
        const IccProfile& source,
        const IccProfile& destination,
        IccRenderingIntent intent,
        bool black_point_compensation
    );
    friend class IccTransform;
};

// Creates an ICC profile whose chromaticities equal sRGB/Rec.709 D65 and whose RGB transfer
// curves are exactly linear. This is the explicit source profile for Shadow's current LibRaw
// processed-reference contract; it must never be confused with display-encoded sRGB.
[[nodiscard]] IccProfile make_linear_srgb_icc_profile();
// Standard display sRGB, including its encoded transfer curve.
[[nodiscard]] IccProfile make_display_srgb_icc_profile();
// Opens a complete `.icc`/`.icm` profile and serializes it for content identity. The source path
// is not retained by the profile, allowing callers to close, replace, or move the original file.
[[nodiscard]] IccProfile load_icc_profile(const std::filesystem::path& path);

class IccTransform final {
public:
    struct State;

    IccTransform();
    IccTransform(const IccTransform&) = default;
    IccTransform& operator=(const IccTransform&) = default;
    IccTransform(IccTransform&&) noexcept = default;
    IccTransform& operator=(IccTransform&&) noexcept = default;
    ~IccTransform();

    // Samples are native-endian `float` triplets in the source profile's RGB encoding. Results
    // replace them in the destination profile's RGB encoding. Non-finite input is rejected before
    // calling LittleCMS so a malformed RAW/Recipe cannot poison a persistent preview cache.
    void apply_interleaved_rgb(std::span<float> samples) const;

    [[nodiscard]] const IccProfileInfo& source() const noexcept;
    [[nodiscard]] const IccProfileInfo& destination() const noexcept;
    [[nodiscard]] IccRenderingIntent intent() const noexcept;

private:
    explicit IccTransform(std::shared_ptr<const State> state);

    std::shared_ptr<const State> state_;

    friend IccTransform make_icc_transform(
        const IccProfile& source,
        const IccProfile& destination,
        IccRenderingIntent intent,
        bool black_point_compensation
    );
};

[[nodiscard]] IccTransform make_icc_transform(
    const IccProfile& source,
    const IccProfile& destination,
    IccRenderingIntent intent = IccRenderingIntent::relative_colorimetric,
    bool black_point_compensation = true
);

} // namespace shadow::image

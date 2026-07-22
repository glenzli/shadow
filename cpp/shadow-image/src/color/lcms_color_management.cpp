#include <shadow/image/color_management.hpp>

#include <lcms2.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace shadow::image {

struct IccProfile::State final {
    cmsHPROFILE profile = nullptr;
    IccProfileInfo info;

    ~State() {
        if (profile != nullptr) {
            cmsCloseProfile(profile);
        }
    }
};

struct IccTransform::State final {
    cmsHTRANSFORM transform = nullptr;
    IccProfileInfo source;
    IccProfileInfo destination;
    IccRenderingIntent intent = IccRenderingIntent::relative_colorimetric;
    mutable std::mutex mutex;

    ~State() {
        if (transform != nullptr) {
            cmsDeleteTransform(transform);
        }
    }
};

namespace {

constexpr std::size_t rgb_channels = 3U;
constexpr std::uint64_t fnv1a_offset_basis = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t fnv1a_prime = 1'099'511'628'211ULL;

[[nodiscard]] std::uint64_t fnv1a64(const std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = fnv1a_offset_basis;
    for (const auto byte : bytes) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= fnv1a_prime;
    }
    return hash;
}

[[nodiscard]] std::string content_id(const std::span<const std::byte> bytes) {
    std::ostringstream stream;
    stream << "icc-fnv1a64-v1:" << std::hex << fnv1a64(bytes) << ";bytes=" << std::dec
           << bytes.size();
    return stream.str();
}

[[nodiscard]] std::vector<std::byte> serialize_profile(cmsHPROFILE profile) {
    cmsUInt32Number bytes = 0U;
    if (cmsSaveProfileToMem(profile, nullptr, &bytes) == 0 || bytes == 0U) {
        throw std::runtime_error("LittleCMS could not size an ICC profile");
    }
    std::vector<std::byte> serialized(bytes);
    if (cmsSaveProfileToMem(profile, serialized.data(), &bytes) == 0
        || bytes != serialized.size()) {
        throw std::runtime_error("LittleCMS could not serialize an ICC profile");
    }
    return serialized;
}

[[nodiscard]] std::string profile_description(cmsHPROFILE profile) {
    const auto bytes = cmsGetProfileInfoASCII(
        profile,
        cmsInfoDescription,
        "en",
        "US",
        nullptr,
        0U
    );
    if (bytes <= 1U) {
        return "Unnamed ICC profile";
    }
    std::string description(bytes, '\0');
    static_cast<void>(cmsGetProfileInfoASCII(
        profile,
        cmsInfoDescription,
        "en",
        "US",
        description.data(),
        static_cast<cmsUInt32Number>(description.size())
    ));
    description.resize(std::char_traits<char>::length(description.c_str()));
    return description.empty() ? std::string{"Unnamed ICC profile"} : description;
}

[[nodiscard]] cmsUInt32Number lcms_intent(const IccRenderingIntent intent) {
    switch (intent) {
    case IccRenderingIntent::perceptual:
        return INTENT_PERCEPTUAL;
    case IccRenderingIntent::relative_colorimetric:
        return INTENT_RELATIVE_COLORIMETRIC;
    case IccRenderingIntent::saturation:
        return INTENT_SATURATION;
    case IccRenderingIntent::absolute_colorimetric:
        return INTENT_ABSOLUTE_COLORIMETRIC;
    }
    throw std::invalid_argument("unsupported ICC rendering intent");
}

[[nodiscard]] cmsHPROFILE make_linear_srgb_profile() {
    const cmsCIExyY white_point{0.3127, 0.3290, 1.0};
    const cmsCIExyYTRIPLE primaries{
        .Red = {0.6400, 0.3300, 1.0},
        .Green = {0.3000, 0.6000, 1.0},
        .Blue = {0.1500, 0.0600, 1.0},
    };
    cmsToneCurve* linear = cmsBuildGamma(nullptr, 1.0);
    if (linear == nullptr) {
        throw std::runtime_error("LittleCMS could not create a linear tone curve");
    }
    std::array<cmsToneCurve*, rgb_channels> curves{linear, linear, linear};
    cmsHPROFILE profile = cmsCreateRGBProfile(&white_point, &primaries, curves.data());
    cmsFreeToneCurve(linear);
    if (profile == nullptr) {
        throw std::runtime_error("LittleCMS could not create a linear sRGB profile");
    }
    return profile;
}

[[nodiscard]] std::shared_ptr<const IccProfile::State> make_profile_state(cmsHPROFILE profile) {
    if (profile == nullptr) {
        throw std::runtime_error("LittleCMS returned a null ICC profile");
    }
    try {
        const auto serialized = serialize_profile(profile);
        auto state = std::make_shared<IccProfile::State>();
        state->profile = profile;
        state->info = {
            .id = content_id(serialized),
            .description = profile_description(profile),
            .serialized_bytes = static_cast<std::uint64_t>(serialized.size()),
        };
        return state;
    } catch (...) {
        cmsCloseProfile(profile);
        throw;
    }
}

} // namespace

IccProfile::IccProfile() = default;
IccProfile::IccProfile(std::shared_ptr<const State> state) : state_(std::move(state)) {}
IccProfile::~IccProfile() = default;

const IccProfileInfo& IccProfile::info() const noexcept {
    static const IccProfileInfo unavailable{
        .id = "icc-unavailable",
        .description = "Unavailable ICC profile",
        .serialized_bytes = 0U,
    };
    return state_ == nullptr ? unavailable : state_->info;
}

IccProfile make_linear_srgb_icc_profile() {
    return IccProfile(make_profile_state(make_linear_srgb_profile()));
}

IccProfile make_display_srgb_icc_profile() {
    return IccProfile(make_profile_state(cmsCreate_sRGBProfile()));
}

IccProfile load_icc_profile(const std::filesystem::path& path) {
    const auto profile = cmsOpenProfileFromFile(path.string().c_str(), "r");
    if (profile == nullptr) {
        throw std::runtime_error("LittleCMS could not open the requested ICC profile");
    }
    return IccProfile(make_profile_state(profile));
}

IccTransform::IccTransform() = default;
IccTransform::IccTransform(std::shared_ptr<const State> state) : state_(std::move(state)) {}
IccTransform::~IccTransform() = default;

void IccTransform::apply_interleaved_rgb(const std::span<float> samples) const {
    if (state_ == nullptr) {
        throw std::logic_error("ICC transform is unavailable");
    }
    if (samples.size() % rgb_channels != 0U) {
        throw std::invalid_argument("ICC transform requires interleaved RGB float triplets");
    }
    for (const auto sample : samples) {
        if (!std::isfinite(sample)) {
            throw std::invalid_argument("ICC transform rejects non-finite RGB samples");
        }
    }
    const auto pixels = samples.size() / rgb_channels;
    if (pixels > std::numeric_limits<cmsUInt32Number>::max()) {
        throw std::overflow_error("ICC transform pixel count exceeds LittleCMS limits");
    }
    std::lock_guard lock(state_->mutex);
    cmsDoTransform(
        state_->transform,
        samples.data(),
        samples.data(),
        static_cast<cmsUInt32Number>(pixels)
    );
}

const IccProfileInfo& IccTransform::source() const noexcept {
    return state_ == nullptr ? IccProfile{}.info() : state_->source;
}

const IccProfileInfo& IccTransform::destination() const noexcept {
    return state_ == nullptr ? IccProfile{}.info() : state_->destination;
}

IccRenderingIntent IccTransform::intent() const noexcept {
    return state_ == nullptr ? IccRenderingIntent::relative_colorimetric : state_->intent;
}

IccTransform make_icc_transform(
    const IccProfile& source,
    const IccProfile& destination,
    const IccRenderingIntent intent,
    const bool black_point_compensation
) {
    if (source.state_ == nullptr || destination.state_ == nullptr) {
        throw std::invalid_argument("ICC transform requires valid source and destination profiles");
    }
    cmsUInt32Number flags = cmsFLAGS_COPY_ALPHA;
    if (black_point_compensation) {
        flags |= cmsFLAGS_BLACKPOINTCOMPENSATION;
    }
    const auto transform = cmsCreateTransform(
        source.state_->profile,
        TYPE_RGB_FLT,
        destination.state_->profile,
        TYPE_RGB_FLT,
        lcms_intent(intent),
        flags
    );
    if (transform == nullptr) {
        throw std::runtime_error("LittleCMS could not create an RGB float transform");
    }
    auto state = std::make_shared<IccTransform::State>();
    state->transform = transform;
    state->source = source.info();
    state->destination = destination.info();
    state->intent = intent;
    return IccTransform(std::move(state));
}

} // namespace shadow::image

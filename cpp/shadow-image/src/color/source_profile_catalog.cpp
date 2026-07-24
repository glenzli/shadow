/*
    The three generic camera-look curves below are derived from darktable's
    GPL-3.0-or-later `src/iop/basecurve.c` at revision
    b0bd5b40816b0e5904fa264542e738760eb0074d. They preserve the original
    project attribution and are recorded in THIRD_PARTY.md. Shadow evaluates
    them with its own luminance-preserving monotone interpolation; it does not
    import darktable's GTK/OpenCL pixelpipe implementation.
*/

#include <shadow/image/source_profile_catalog.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace shadow::image {

namespace {

inline constexpr std::string_view source_profile_header = "shadow-source-profile-v1";
inline constexpr std::string_view source_profile_extension = ".shadow-source-profile";
inline constexpr std::uintmax_t maximum_source_profile_bytes = 64U * 1024U;
inline constexpr std::size_t maximum_source_tone_curve_points = 32U;
inline constexpr std::string_view generic_camera_model = "*";

[[nodiscard]] std::string trim_ascii(const std::string_view value) {
    std::size_t first = 0U;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

[[nodiscard]] std::string normalized_text(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    bool pending_space = false;
    for (const char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) != 0) {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space) {
            result.push_back(' ');
            pending_space = false;
        }
        result.push_back(static_cast<char>(std::tolower(byte)));
    }
    return result;
}

[[nodiscard]] bool is_safe_identifier(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 128U) {
        return false;
    }
    return std::ranges::all_of(value, [](const char character) {
        const unsigned char byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '-' || character == '_' || character == '.';
    });
}

[[nodiscard]] std::uint64_t fnv1a64(const std::string_view text) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

[[nodiscard]] std::string content_identity(const std::string_view content) {
    std::ostringstream stream;
    stream << "fnv1a64-v1:" << std::hex << fnv1a64(content);
    return stream.str();
}

[[nodiscard]] std::optional<double> parse_finite_double(const std::string_view value) {
    double parsed = 0.0;
    const auto [cursor, error] = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed
    );
    if (error != std::errc{} || cursor != value.data() + value.size() || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<std::vector<SourceToneCurvePoint>> parse_tone_curve(
    const std::string_view text
) {
    std::vector<SourceToneCurvePoint> points;
    std::size_t begin = 0U;
    while (begin < text.size()) {
        const std::size_t end = text.find(',', begin);
        const std::string point_text = trim_ascii(text.substr(
            begin,
            end == std::string_view::npos ? std::string_view::npos : end - begin
        ));
        const std::size_t separator = point_text.find(':');
        if (point_text.empty() || separator == std::string::npos
            || point_text.find(':', separator + 1U) != std::string::npos) {
            return std::nullopt;
        }
        const auto input = parse_finite_double(trim_ascii(
            std::string_view(point_text).substr(0U, separator)
        ));
        const auto output = parse_finite_double(trim_ascii(
            std::string_view(point_text).substr(separator + 1U)
        ));
        if (!input.has_value() || !output.has_value() || *input < 0.0 || *input > 1.0
            || *output < 0.0 || *output > 1.0 || points.size() >= maximum_source_tone_curve_points
            || (!points.empty() && (*input <= points.back().input || *output < points.back().output))) {
            return std::nullopt;
        }
        points.push_back(SourceToneCurvePoint{.input = *input, .output = *output});
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    if (points.size() < 2U || points.front().input != 0.0 || points.back().input != 1.0) {
        return std::nullopt;
    }
    return points;
}

[[nodiscard]] bool has_valid_tone_curve(
    const std::vector<SourceToneCurvePoint>& points
) noexcept {
    if (points.empty()) {
        return true;
    }
    if (points.size() < 2U || points.size() > maximum_source_tone_curve_points
        || points.front().input != 0.0 || points.back().input != 1.0) {
        return false;
    }
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const auto& point = points[index];
        if (!std::isfinite(point.input) || !std::isfinite(point.output) || point.input < 0.0
            || point.input > 1.0 || point.output < 0.0 || point.output > 1.0) {
            return false;
        }
        if (index != 0U && (point.input <= points[index - 1U].input
            || point.output < points[index - 1U].output)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<SourceProfileDefinition> parse_source_profile(
    const std::string_view document
) {
    std::istringstream lines{std::string(document)};
    std::string line;
    if (!std::getline(lines, line) || trim_ascii(line) != source_profile_header) {
        return std::nullopt;
    }

    std::string id;
    std::string display_name;
    std::string make;
    std::string model;
    std::optional<double> exposure;
    std::optional<std::vector<SourceToneCurvePoint>> tone_curve;
    bool saw_tone_curve = false;
    while (std::getline(lines, line)) {
        const std::string trimmed = trim_ascii(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const std::size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            return std::nullopt;
        }
        const std::string key = trim_ascii(std::string_view(trimmed).substr(0U, separator));
        const std::string value = trim_ascii(std::string_view(trimmed).substr(separator + 1U));
        if (key == "id" && id.empty()) {
            id = value;
        } else if (key == "name" && display_name.empty()) {
            display_name = value;
        } else if (key == "make" && make.empty()) {
            make = value;
        } else if (key == "model" && model.empty()) {
            model = value;
        } else if (key == "exposure_stops" && !exposure.has_value()) {
            exposure = parse_finite_double(value);
        } else if (key == "tone_curve" && !saw_tone_curve) {
            saw_tone_curve = true;
            tone_curve = parse_tone_curve(value);
            if (!tone_curve.has_value()) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }

    if (!is_safe_identifier(id) || display_name.empty() || display_name.size() > 256U
        || make.empty() || model.empty() || !exposure.has_value()
        || std::abs(*exposure) > 8.0 || (tone_curve.has_value() && !has_valid_tone_curve(*tone_curve))) {
        return std::nullopt;
    }
    const std::string normalized_make = normalized_text(make);
    const std::string normalized_model = normalized_text(model);
    if (normalized_make.empty() || normalized_model.empty()) {
        return std::nullopt;
    }
    return SourceProfileDefinition{
        .id = std::move(id),
        .display_name = std::move(display_name),
        .normalized_make = normalized_make,
        .normalized_model = normalized_model,
        .display_exposure_stops = *exposure,
        .has_exposure_calibration = true,
        .luminance_tone_curve = tone_curve.value_or(std::vector<SourceToneCurvePoint>{}),
        .content_identity = content_identity(document),
    };
}

[[nodiscard]] std::optional<std::string> read_source_profile_document(
    const std::filesystem::path& path
) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size == 0U || size > maximum_source_profile_bytes) {
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string content(static_cast<std::size_t>(size), '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (file.gcount() != static_cast<std::streamsize>(content.size())) {
        return std::nullopt;
    }
    return content;
}

[[nodiscard]] std::filesystem::path configured_source_profile_directory() {
    const auto* environment = std::getenv("SHADOW_SOURCE_PROFILE_DIRECTORY");
    if (environment != nullptr && *environment != '\0') {
        return std::filesystem::path(environment);
    }
    const auto* home = std::getenv("HOME");
#if defined(_WIN32)
    const auto* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data != nullptr && *local_app_data != '\0') {
        return std::filesystem::path(local_app_data) / "Shadow" / "source-profiles";
    }
    return {};
#elif defined(__APPLE__)
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return std::filesystem::path(home) / "Library" / "Application Support" / "Shadow"
        / "source-profiles";
#else
    const auto* xdg_data_home = std::getenv("XDG_DATA_HOME");
    if (xdg_data_home != nullptr && *xdg_data_home != '\0') {
        return std::filesystem::path(xdg_data_home) / "shadow" / "source-profiles";
    }
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return std::filesystem::path(home) / ".local" / "share" / "shadow" / "source-profiles";
#endif
}

[[nodiscard]] std::string catalog_identity(const std::vector<SourceProfileDefinition>& profiles) {
    std::ostringstream stream;
    stream << "shadow-source-profile-catalog-v" << source_profile_document_schema_version;
    for (const auto& profile : profiles) {
        stream << ';' << profile.id << '=' << profile.content_identity;
    }
    return content_identity(stream.str());
}

[[nodiscard]] SourceProfileDefinition darktable_basecurve_profile(
    const std::string_view id,
    const std::string_view display_name,
    const std::string_view make,
    std::vector<SourceToneCurvePoint> curve
) {
    return SourceProfileDefinition{
        .id = std::string(id),
        .display_name = std::string(display_name),
        .normalized_make = normalized_text(make),
        .normalized_model = std::string(generic_camera_model),
        .display_exposure_stops = 0.0,
        .has_exposure_calibration = false,
        .luminance_tone_curve = std::move(curve),
        .content_identity = "darktable-basecurve:b0bd5b40816b0e5904fa264542e738760eb0074d:"
            + std::string(id),
    };
}

[[nodiscard]] std::vector<SourceProfileDefinition> builtin_source_profiles() {
    // These are darktable's generic maker fallbacks, not camera-specific
    // measurements. Exact local profiles deliberately win during matching.
    return {
        darktable_basecurve_profile(
            "darktable-canon-eos-like",
            "Open Camera Look · Canon",
            "Canon",
            {
                {0.000000, 0.000000}, {0.028226, 0.029677}, {0.120968, 0.232258},
                {0.459677, 0.747581}, {0.858871, 0.967742}, {1.000000, 1.000000},
            }
        ),
        darktable_basecurve_profile(
            "darktable-nikon-like",
            "Open Camera Look · Nikon",
            "Nikon",
            {
                {0.000000, 0.000000}, {0.036290, 0.036532}, {0.120968, 0.228226},
                {0.459677, 0.759678}, {0.858871, 0.983468}, {1.000000, 1.000000},
            }
        ),
        darktable_basecurve_profile(
            "darktable-sony-alpha-like",
            "Open Camera Look · Sony Alpha",
            "Sony",
            {
                {0.000000, 0.000000}, {0.031949, 0.036532}, {0.105431, 0.228226},
                {0.434505, 0.759678}, {0.855738, 0.983468}, {1.000000, 1.000000},
            }
        ),
    };
}

void canonicalize_catalog(SourceProfileCatalog& catalog) {
    std::sort(
        catalog.profiles.begin(),
        catalog.profiles.end(),
        [](const SourceProfileDefinition& left, const SourceProfileDefinition& right) {
            return left.id < right.id;
        }
    );
    catalog.identity = catalog_identity(catalog.profiles);
}

} // namespace

SourceProfileCatalog load_source_profile_catalog(const std::filesystem::path& directory) {
    SourceProfileCatalog catalog;
    std::error_code error;
    if (directory.empty() || !std::filesystem::is_directory(directory, error) || error) {
        catalog.identity = catalog_identity(catalog.profiles);
        return catalog;
    }

    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        const auto& entry = *iterator;
        if (!entry.is_regular_file(error) || error
            || entry.path().extension() != source_profile_extension) {
            error.clear();
            continue;
        }
        candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& path : candidates) {
        const auto document = read_source_profile_document(path);
        if (!document.has_value()) {
            continue;
        }
        const auto profile = parse_source_profile(*document);
        if (!profile.has_value()) {
            continue;
        }
        const bool duplicate_id = std::ranges::any_of(
            catalog.profiles,
            [&profile](const SourceProfileDefinition& existing) {
                return existing.id == profile->id;
            }
        );
        if (!duplicate_id) {
            catalog.profiles.push_back(*profile);
        }
    }
    canonicalize_catalog(catalog);
    return catalog;
}

SourceProfileCatalog load_builtin_source_profile_catalog() {
    SourceProfileCatalog catalog{.profiles = builtin_source_profiles()};
    canonicalize_catalog(catalog);
    return catalog;
}

SourceProfileCatalog load_local_source_profile_catalog() {
    SourceProfileCatalog catalog = load_builtin_source_profile_catalog();
    const SourceProfileCatalog local = load_source_profile_catalog(configured_source_profile_directory());
    for (const auto& profile : local.profiles) {
        const auto same_id = std::find_if(
            catalog.profiles.begin(),
            catalog.profiles.end(),
            [&profile](const SourceProfileDefinition& existing) {
                return existing.id == profile.id;
            }
        );
        if (same_id != catalog.profiles.end()) {
            *same_id = profile;
        } else {
            catalog.profiles.push_back(profile);
        }
    }
    canonicalize_catalog(catalog);
    return catalog;
}

std::optional<SourceProfileDefinition> match_source_profile(
    const SourceProfileCatalog& catalog,
    const AssetMetadata& metadata
) {
    const std::string make = metadata.normalized_make.empty()
        ? normalized_text(metadata.make)
        : normalized_text(metadata.normalized_make);
    const std::string model = metadata.normalized_model.empty()
        ? normalized_text(metadata.model)
        : normalized_text(metadata.normalized_model);
    if (make.empty() || model.empty()) {
        return std::nullopt;
    }
    const auto exact_match = std::find_if(
        catalog.profiles.begin(),
        catalog.profiles.end(),
        [&make, &model](const SourceProfileDefinition& profile) {
            return profile.normalized_make == make && profile.normalized_model == model;
        }
    );
    if (exact_match != catalog.profiles.end()) {
        return *exact_match;
    }
    const auto maker_fallback = std::find_if(
        catalog.profiles.begin(),
        catalog.profiles.end(),
        [&make](const SourceProfileDefinition& profile) {
            return profile.normalized_make == make && profile.normalized_model == generic_camera_model;
        }
    );
    return maker_fallback == catalog.profiles.end()
        ? std::nullopt
        : std::optional<SourceProfileDefinition>(*maker_fallback);
}

} // namespace shadow::image

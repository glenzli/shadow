#include "contract_test_assertions.hpp"

#include "../src/optics/lensfun_profile_catalog.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <tuple>

namespace image = shadow::image;
namespace catalog = shadow::image::lensfun_profile_catalog;

namespace {

using shadow::image::test_support::expect;
using shadow::image::test_support::failures;

[[nodiscard]] image::AssetMetadata nikon_d850_metadata() {
    image::AssetMetadata metadata;
    metadata.make = "Nikon Corporation";
    metadata.model = "Nikon D850";
    metadata.normalized_make = "Nikon";
    metadata.normalized_model = "D850";
    metadata.lens_make = "Nikon";
    metadata.lens_model = "Nikon AF Nikkor 50mm f/1.4D";
    metadata.focal_length_mm = 50.0;
    metadata.focal_length_35mm = 50.0;
    metadata.aperture_f_number = 1.4;
    metadata.focus_distance_meters = 10.0;
    return metadata;
}

void unavailable_catalog_has_an_explicit_identity() {
    const auto missing_database = std::filesystem::temp_directory_path()
        / "shadow-lensfun-profile-catalog-intentionally-missing";
    const catalog::Catalog profiles(missing_database);
    expect(profiles.info().id == "lensfun",
           "an unavailable profile catalog retains the Lensfun provider identity");
    expect(!profiles.info().available && !profiles.info().version.empty(),
           "a missing profile database is represented explicitly");
    expect(profiles.profile_candidates(nikon_d850_metadata()).empty(),
           "an unavailable profile catalog exposes no candidates");
}

void profile_identity_admission_distinguishes_automatic_and_manual_selection() {
    const catalog::Catalog profiles(
        std::filesystem::temp_directory_path()
        / "shadow-lensfun-profile-catalog-intentionally-missing");
    const auto defaults = image::default_optics_settings();
    expect(!profiles.profile_identity_available(image::AssetMetadata{}, defaults),
           "automatic profile discovery requires camera and lens identity");

    auto automatic = nikon_d850_metadata();
    automatic.make.clear();
    automatic.model.clear();
    expect(profiles.profile_identity_available(automatic, defaults),
           "normalized camera identity is sufficient for automatic matching");

    auto manual = defaults;
    manual.camera_profile_model = "Nikon D850";
    manual.lens_profile_model = "Nikon AF Nikkor 50mm f/1.4D";
    expect(profiles.profile_identity_available(image::AssetMetadata{}, manual),
           "manual profile identity does not depend on EXIF camera or lens text");
}

void real_database_discovery_and_resolution_are_stable_when_available() {
    const auto* database = std::getenv("SHADOW_TEST_LENSFUN_DB");
    if (database == nullptr || *database == '\0') {
        return;
    }
    const catalog::Catalog profiles(std::filesystem::path{database});
    expect(profiles.info().available,
           "the configured Lensfun profile catalog loads");
    if (!profiles.info().available) {
        return;
    }

    const auto metadata = nikon_d850_metadata();
    const auto candidates = profiles.profile_candidates(metadata);
    expect(!candidates.empty(),
           "the matched camera projects compatible Lensfun profiles");
    expect(
        std::ranges::is_sorted(candidates, [](const auto& left, const auto& right) {
            return std::tie(left.lens_maker, left.lens_model)
                < std::tie(right.lens_maker, right.lens_model);
        }),
        "compatible profiles have a stable maker/model order"
    );
    expect(
        std::adjacent_find(
            candidates.begin(),
            candidates.end(),
            [](const auto& left, const auto& right) {
                return left.camera_maker == right.camera_maker
                    && left.camera_model == right.camera_model
                    && left.lens_maker == right.lens_maker
                    && left.lens_model == right.lens_model;
            }
        ) == candidates.end(),
        "compatible profile projection removes duplicate identities"
    );

    const auto defaults = image::default_optics_settings();
    auto first = std::async(std::launch::async, [&] {
        return profiles.resolve_profile(metadata, defaults);
    });
    auto second = std::async(std::launch::async, [&] {
        return profiles.resolve_profile(metadata, defaults);
    });
    const auto first_resolution = first.get();
    const auto second_resolution = second.get();
    expect(
        first_resolution.status == image::OpticsProfileStatus::matched
            && first_resolution.match.has_value()
            && second_resolution.status == image::OpticsProfileStatus::matched
            && second_resolution.match.has_value(),
        "concurrent resolution safely shares the profile match cache"
    );
    if (!first_resolution.match.has_value() || candidates.empty()) {
        return;
    }
    expect(
        first_resolution.match->camera_name == second_resolution.match->camera_name
            && first_resolution.match->lens_name == second_resolution.match->lens_name,
        "cached resolutions retain stable camera and lens identities"
    );

    auto missing_camera = metadata;
    missing_camera.make = "Shadow Missing Camera";
    missing_camera.model = "Shadow Missing Model";
    missing_camera.normalized_make.clear();
    missing_camera.normalized_model.clear();
    expect(
        profiles.resolve_profile(missing_camera, defaults).status
            == image::OpticsProfileStatus::camera_not_found,
        "an absent camera has an explicit catalog status"
    );

    auto missing_lens = metadata;
    missing_lens.lens_make = "Shadow";
    missing_lens.lens_model = "Shadow Missing Lens";
    expect(
        profiles.resolve_profile(missing_lens, defaults).status
            == image::OpticsProfileStatus::lens_not_found,
        "an absent lens has an explicit catalog status"
    );

    auto manual = defaults;
    manual.camera_profile_maker = candidates.front().camera_maker;
    manual.camera_profile_model = candidates.front().camera_model;
    manual.lens_profile_maker = candidates.front().lens_maker;
    manual.lens_profile_model = candidates.front().lens_model;
    expect(
        profiles.resolve_profile(image::AssetMetadata{}, manual).status
            == image::OpticsProfileStatus::matched,
        "explicit profile identities resolve without EXIF identity"
    );
}

} // namespace

int main() {
    unavailable_catalog_has_an_explicit_identity();
    profile_identity_admission_distinguishes_automatic_and_manual_selection();
    real_database_discovery_and_resolution_are_stable_when_available();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

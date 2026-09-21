find_package(
    Qt6 6.11.1 REQUIRED
    COMPONENTS Core Concurrent LinguistTools Network Quick QuickControls2 Svg WebView Widgets
)
find_package(PkgConfig REQUIRED)
find_package(JPEG REQUIRED)
set(
    SHADOW_VCPKG_ICONV_PREFIX
    ""
    CACHE PATH
    "Optional vcpkg Iconv prefix used when the port installs beneath usr/local"
)
if(WIN32 AND SHADOW_VCPKG_ICONV_PREFIX)
    if(EXISTS "${SHADOW_VCPKG_ICONV_PREFIX}")
        list(PREPEND CMAKE_PREFIX_PATH "${SHADOW_VCPKG_ICONV_PREFIX}")
        list(PREPEND CMAKE_INCLUDE_PATH "${SHADOW_VCPKG_ICONV_PREFIX}/include")
        list(
            PREPEND CMAKE_LIBRARY_PATH
            "${SHADOW_VCPKG_ICONV_PREFIX}/lib"
            "${SHADOW_VCPKG_ICONV_PREFIX}/debug/lib"
        )
        message(STATUS "Using vcpkg Iconv compatibility prefix: ${SHADOW_VCPKG_ICONV_PREFIX}")
    else()
        message(STATUS
            "Configured vcpkg Iconv compatibility prefix is absent; "
            "using the canonical vcpkg layout"
        )
    endif()
endif()
find_package(Iconv REQUIRED)
find_package(TIFF REQUIRED)
# Keep the final executable on the same reentrant LibRaw selected by the C++ kernel and Rust bridge.
pkg_check_modules(LibRaw REQUIRED IMPORTED_TARGET "libraw_r>=0.22.0")
if(NOT "raw_r" IN_LIST LibRaw_LIBRARIES OR "raw" IN_LIST LibRaw_LIBRARIES)
    message(FATAL_ERROR
        "Shadow requires pkg-config libraw_r to resolve to the reentrant -lraw_r library; "
        "resolved libraries: ${LibRaw_LIBRARIES}"
    )
endif()
pkg_check_modules(Lensfun QUIET IMPORTED_TARGET lensfun)
pkg_check_modules(LittleCms REQUIRED IMPORTED_TARGET lcms2)
pkg_check_modules(LibHeif QUIET IMPORTED_TARGET libheif>=1.19.7)

qt_standard_project_setup(
    REQUIRES 6.11
    I18N_SOURCE_LANGUAGE en
    I18N_TRANSLATED_LANGUAGES zh_CN
)

set(
    SHADOW_GEONAMES_CITY_INDEX_PATH
    ""
    CACHE FILEPATH
    "Prepared GeoNames city index copied into the Shadow application bundle"
)
if(SHADOW_GEONAMES_CITY_INDEX_PATH AND NOT EXISTS "${SHADOW_GEONAMES_CITY_INDEX_PATH}")
    message(FATAL_ERROR
        "SHADOW_GEONAMES_CITY_INDEX_PATH does not exist: ${SHADOW_GEONAMES_CITY_INDEX_PATH}"
    )
endif()
if(APPLE)
    set(
        SHADOW_GEONAMES_BUNDLE_DIRECTORY
        "$<TARGET_BUNDLE_DIR:shadow-desktop>/Contents/Resources/GeoNames"
    )
else()
    set(
        SHADOW_GEONAMES_BUNDLE_DIRECTORY
        "$<TARGET_FILE_DIR:shadow-desktop>/GeoNames"
    )
endif()

find_program(SHADOW_CARGO_EXECUTABLE cargo REQUIRED)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(SHADOW_CARGO_PROFILE release)
    set(SHADOW_CARGO_PROFILE_ARGUMENT --release)
else()
    set(SHADOW_CARGO_PROFILE debug)
    set(SHADOW_CARGO_PROFILE_ARGUMENT)
endif()
if(APPLE AND SHADOW_ENABLE_METAL)
    set(SHADOW_CARGO_NATIVE_VARIANT metal)
else()
    set(SHADOW_CARGO_NATIVE_VARIANT cpu)
endif()

set(
    SHADOW_CARGO_TARGET_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/cargo-target-${SHADOW_CARGO_NATIVE_VARIANT}"
)

file(
    GLOB_RECURSE SHADOW_RUST_BRIDGE_INPUTS
    CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/Cargo.toml"
    "${PROJECT_SOURCE_DIR}/Cargo.lock"
    "${PROJECT_SOURCE_DIR}/crates/*/Cargo.toml"
    "${PROJECT_SOURCE_DIR}/crates/*/build.rs"
    "${PROJECT_SOURCE_DIR}/crates/*/src/*.rs"
    "${PROJECT_SOURCE_DIR}/crates/*/src/**/*.rs"
    "${PROJECT_SOURCE_DIR}/cpp/shadow-image/include/**/*.hpp"
    "${PROJECT_SOURCE_DIR}/cpp/shadow-image/src/bridge/*.cpp"
)

set(
    SHADOW_DESKTOP_RUST_LIBRARY
    "${SHADOW_CARGO_TARGET_DIRECTORY}/${SHADOW_CARGO_PROFILE}/${CMAKE_STATIC_LIBRARY_PREFIX}shadow_desktop_bridge${CMAKE_STATIC_LIBRARY_SUFFIX}"
)
set(
    SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY
    "${SHADOW_CARGO_TARGET_DIRECTORY}/cxxbridge"
)
if(UNIX)
    # `cmake -E env` closes GNU Make's jobserver descriptors before launching Cargo. Use the
    # system `env` executable on Unix so JOB_SERVER_AWARE can preserve bounded parallelism.
    find_program(SHADOW_ENV_EXECUTABLE env REQUIRED)
    set(SHADOW_CARGO_ENV "${SHADOW_ENV_EXECUTABLE}")
else()
    set(SHADOW_CARGO_ENV "${CMAKE_COMMAND}" -E env)
endif()
# CMake owns the desktop image implementation. Cargo contributes only Rust plus the generated CXX
# ABI shim, so native implementation translation units are compiled exactly once as Shadow::Image.
list(APPEND SHADOW_CARGO_ENV SHADOW_BRIDGE_EXTERNAL_IMAGE=1)
if(APPLE)
    # Homebrew's Qt 6.11 frameworks currently target macOS 26.0. Match the
    # Rust static library so the final app does not raise mixed-minimum warnings.
    list(APPEND SHADOW_CARGO_ENV MACOSX_DEPLOYMENT_TARGET=26.0)
endif()
if(APPLE AND SHADOW_ENABLE_METAL)
    list(APPEND SHADOW_CARGO_ENV SHADOW_ENABLE_METAL=1)
else()
    list(APPEND SHADOW_CARGO_ENV SHADOW_ENABLE_METAL=0)
endif()
add_custom_command(
    OUTPUT "${SHADOW_DESKTOP_RUST_LIBRARY}"
    BYPRODUCTS
        "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}/shadow-desktop-bridge/src/lib.rs.h"
    COMMAND
        ${SHADOW_CARGO_ENV} "${SHADOW_CARGO_EXECUTABLE}" build
        --package shadow-desktop-bridge
        ${SHADOW_CARGO_PROFILE_ARGUMENT}
        --target-dir "${SHADOW_CARGO_TARGET_DIRECTORY}"
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSHADOW_CARGO_TARGET_DIRECTORY=${SHADOW_CARGO_TARGET_DIRECTORY}"
        "-DSHADOW_CARGO_PROFILE=${SHADOW_CARGO_PROFILE}"
        "-DSHADOW_CXXBRIDGE_INCLUDE_DIRECTORY=${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sync_cxxbridge_headers.cmake"
    DEPENDS ${SHADOW_RUST_BRIDGE_INPUTS}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Building the Rust desktop snapshot bridge"
    JOB_SERVER_AWARE TRUE
    VERBATIM
)
add_custom_target(shadow-desktop-rust-build DEPENDS "${SHADOW_DESKTOP_RUST_LIBRARY}")
add_library(shadow-desktop-rust STATIC IMPORTED GLOBAL)
set_target_properties(
    shadow-desktop-rust
    PROPERTIES IMPORTED_LOCATION "${SHADOW_DESKTOP_RUST_LIBRARY}"
)
add_dependencies(shadow-desktop-rust shadow-desktop-rust-build)
target_link_libraries(
    shadow-desktop-rust
    INTERFACE Shadow::ImagePlatformRuntime
)

add_library(
    shadow-desktop-export-backend
    OBJECT
    src/backend/export_backend.cpp
    src/backend/export_backend.hpp
    src/backend/export_raster_encoder.cpp
    src/backend/export_raster_encoder.hpp
    src/backend/export_settings_codec.cpp
    src/backend/export_settings_codec.hpp
)
target_compile_features(shadow-desktop-export-backend PUBLIC cxx_std_20)
target_include_directories(
    shadow-desktop-export-backend
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
)
target_link_libraries(
    shadow-desktop-export-backend
    PUBLIC
        shadow-desktop-rust
        Shadow::Image
        PkgConfig::LibRaw
        PkgConfig::LittleCms
        ${CMAKE_DL_LIBS}
        JPEG::JPEG
        TIFF::TIFF
        Iconv::Iconv
        Qt6::Core
        Qt6::Gui
)
add_dependencies(
    shadow-desktop-export-backend
    shadow-desktop-rust-build
)
if(Lensfun_FOUND)
    target_link_libraries(
        shadow-desktop-export-backend
        PUBLIC PkgConfig::Lensfun
    )
endif()
if(LibHeif_FOUND)
    target_link_libraries(
        shadow-desktop-export-backend
        PUBLIC PkgConfig::LibHeif
    )
endif()
if(MSVC)
    target_compile_options(
        shadow-desktop-export-backend
        PRIVATE /W4 /permissive-
    )
else()
    target_compile_options(
        shadow-desktop-export-backend
        PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()

set(
    SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES
    src/backend/edit_settings_projection.cpp
    src/backend/edit_settings_projection.hpp
    src/backend/rust_qt_projection.hpp
)

set(
    SHADOW_DESKTOP_HISTORY_PROJECTION_SOURCES
    src/backend/history_projection.cpp
    src/backend/history_projection.hpp
    src/backend/history_types.hpp
)

include(cmake/ShadowDesktopPlatformSources.cmake)

add_library(
    shadow-library-server-control
    STATIC
    src/backend/library_server_projection.cpp
    src/backend/library_server_projection.hpp
    src/backend/library_server_types.hpp
    src/library_server_controller.cpp
    src/library_server_controller.hpp
    src/library_server_host.cpp
    src/library_server_host.hpp
    src/secure_secret_store.cpp
    src/secure_secret_store.hpp
)
target_compile_features(shadow-library-server-control PUBLIC cxx_std_20)
target_include_directories(
    shadow-library-server-control
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
)
target_link_libraries(
    shadow-library-server-control
    PUBLIC
        Qt6::Concurrent
        Qt6::Core
        Qt6::Gui
        Qt6::Network
    PRIVATE
        shadow-desktop-rust
        Shadow::Image
        PkgConfig::LibRaw
        PkgConfig::LittleCms
        ${CMAKE_DL_LIBS}
        JPEG::JPEG
        Iconv::Iconv
)
add_dependencies(shadow-library-server-control shadow-desktop-rust-build)
if(MSVC)
    target_compile_options(
        shadow-library-server-control
        PRIVATE /W4 /permissive-
    )
else()
    target_compile_options(
        shadow-library-server-control
        PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()

qt_add_executable(
    shadow-desktop
    MACOSX_BUNDLE
    ${SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES}
    ${SHADOW_DESKTOP_HISTORY_PROJECTION_SOURCES}
    src/desktop_backend.cpp
    src/desktop_backend_ai_mask.cpp
    src/desktop_backend_ai_completion.cpp
    src/desktop_backend_raw_foundation.cpp
    src/desktop_backend_cache.cpp
    src/desktop_backend_edit.cpp
    src/desktop_backend_lut_export.cpp
    src/backend/lut_export_types.hpp
    src/lut_export_controller.cpp
    src/lut_export_controller.hpp
    src/desktop_backend_history.cpp
    src/desktop_backend_image_understanding.cpp
    src/desktop_backend_library.cpp
    src/desktop_backend_library_server.cpp
    src/desktop_backend_people.cpp
    src/desktop_backend_semantic_search.cpp
    src/desktop_backend_smart_classification.cpp
    src/desktop_backend_remote_library.cpp
    src/desktop_backend_review.cpp
    src/desktop_backend.hpp
    src/backend/desktop_backend_private.hpp
    src/backend/edit_preview_frame.hpp
    src/backend/raw_foundation_projection.cpp
    src/backend/raw_foundation_projection.hpp
    src/backend/rust_owned_edit_preview_frame.cpp
    src/backend/rust_owned_edit_preview_frame.hpp
    src/backend/review_projection.hpp
    src/backend/cache_types.hpp
    src/backend/edit_types.hpp
    src/backend/image_understanding_types.hpp
    src/backend/library_types.hpp
    src/backend/library_server_types.hpp
    src/backend/remote_library_types.hpp
    src/backend/people_analysis_types.hpp
    src/backend/semantic_search_types.hpp
    src/backend/smart_category_types.hpp
    src/backend/review_types.hpp
    src/folder_scan_backend.cpp
    src/folder_scan_backend.hpp
    src/desktop_smoke_harness.cpp
    src/desktop_smoke_harness.hpp
    src/desktop_smoke/application_lifecycle.cpp
    src/desktop_smoke/application_lifecycle.hpp
    src/desktop_smoke/edit_preview_session.cpp
    src/desktop_smoke/edit_preview_session.hpp
    src/desktop_smoke/grade_stack_persistence.cpp
    src/desktop_smoke/grade_stack_persistence.hpp
    src/desktop_smoke/library_lifecycle.cpp
    src/desktop_smoke/library_lifecycle.hpp
    src/desktop_smoke/review_mutations.cpp
    src/desktop_smoke/review_mutations.hpp
    src/desktop_smoke/scenario_setup.cpp
    src/desktop_smoke/scenario_setup.hpp
    src/desktop_smoke/visual_capture.cpp
    src/desktop_smoke/visual_capture.hpp
    src/cache_maintenance_controller.cpp
    src/cache_maintenance_controller.hpp
    src/edit_controller.cpp
    src/edit_interchange_controller.cpp
    src/edit_interchange_controller.hpp
    src/edit_interchange_application.cpp
    src/xmp_develop_import.cpp
    src/xmp_develop_import.hpp
    src/edit_variant_controller.cpp
    src/edit_adjustment_controller.cpp
    src/edit_adjustment_reset_controller.cpp
    src/subject_emphasis_smoke_harness.cpp
    src/subject_emphasis_smoke_harness.hpp
    src/edit_paint_controller.cpp
    src/edit_paint_controller.hpp
    src/paint_brush_presets.cpp
    src/paint_brush_presets.hpp
    src/paint_stroke_input.cpp
    src/paint_stroke_input.hpp
    src/edit_subject_emphasis_controller.cpp
    src/edit_subject_emphasis_controller.hpp
    src/edit_ai_mask_controller.cpp
    src/edit_ai_mask_controller.hpp
    src/edit_ai_completion_controller.cpp
    src/edit_ai_completion_controller.hpp
    src/edit_auto_geometry_controller.cpp
    src/edit_auto_geometry_controller.hpp
    src/edit_ai_mask_prompt_state.cpp
    src/edit_ai_mask_prompt_state.hpp
    src/edit_analysis_controller.cpp
    src/edit_raw_foundation_controller.cpp
    src/edit_raw_foundation_controller.hpp
    src/edit_processing_stack_controller.cpp
    src/edit_raw_foundation_state.cpp
    src/edit_raw_foundation_state.hpp
    src/edit_detail_render_controller.cpp
    src/edit_controller.hpp
    src/edit_fine_parameter_registry.cpp
    src/edit_fine_parameter_registry.hpp
    src/edit_point_color_controller.cpp
    src/edit_point_color_model.hpp
    src/edit_foundation_controller.cpp
    src/edit_geometry_controller.cpp
    src/edit_grade_node_controller.cpp
    src/edit_history_controller.cpp
    src/edit_history_restore_projection.cpp
    src/edit_local_mask_controller.cpp
    src/edit_condition_mask_controller.cpp
    src/edit_liquify_coordinates.cpp
    src/edit_liquify_coordinates.hpp
    src/edit_liquify_controller.cpp
    src/edit_mask_coverage_contract.hpp
    src/edit_mask_coverage_controller.cpp
    src/edit_mask_coverage_store.cpp
    src/edit_mask_assignment_controller.cpp
    src/edit_optics_controller.cpp
    src/edit_persistence_coordinator.cpp
    src/edit_persistence_state.cpp
    src/edit_persistence_state.hpp
    src/edit_persistence_task_coordinator.cpp
    src/edit_persistence_task_coordinator.hpp
    src/edit_render_coordinator.cpp
    src/edit_retouch_controller.cpp
    src/edit_retouch_donor_selection.cpp
    src/edit_retouch_donor_selection.hpp
    src/edit_retouch_donor_preview.cpp
    src/edit_retouch_donor_preview.hpp
    src/edit_retouch_sources.cpp
    src/edit_retouch_sources.hpp
    src/edit_retouch_coordinates.cpp
    src/edit_retouch_coordinates.hpp
    src/edit_retouch_projection.cpp
    src/edit_source_admission.cpp
    src/edit_source_admission.hpp
    src/edit_stroke_input.cpp
    src/edit_stroke_input.hpp
    src/edit_task_runner.cpp
    src/edit_task_runner.hpp
    src/edit_tone_curve_controller.cpp
    src/edit_history.hpp
    src/edit_preview_contract.hpp
    src/edit_before_preview_state.hpp
    src/edit_preview_liquify_mesh.cpp
    src/edit_preview_liquify_mesh.hpp
    ${SHADOW_DESKTOP_PLATFORM_SOURCES}
    src/edit_preview_presentation_context.cpp
    src/edit_preview_presentation_context.hpp
    src/edit_preview_presentation_registry.cpp
    src/edit_preview_presentation_registry.hpp
    src/edit_preview_provider.cpp
    src/edit_preview_provider.hpp
    src/edit_preview_store.cpp
    src/edit_preview_texture_item.cpp
    src/edit_preview_texture_item.hpp
    src/edit_stack.hpp
    src/edit_version_model.cpp
    src/edit_version_model.hpp
    src/edit_version_presentation.cpp
    src/edit_version_presentation.hpp
    src/composition_controller.cpp
    src/composition_controller.hpp
    src/export_controller.cpp
    src/export_controller.hpp
    src/export_preset_store.cpp
    src/export_preset_store.hpp
    src/export_watermark_store.cpp
    src/export_watermark_store.hpp
    src/export_task_runner.cpp
    src/export_task_runner.hpp
    src/default_library_reverse_geocoder.cpp
    src/default_library_reverse_geocoder.hpp
    src/amap_coordinate_transform.cpp
    src/amap_coordinate_transform.hpp
    src/amap_web_service_protocol.cpp
    src/amap_web_service_protocol.hpp
    src/amap_library_reverse_geocoder.cpp
    src/amap_library_reverse_geocoder.hpp
    src/amap_place_search_service.cpp
    src/amap_place_search_service.hpp
    src/geonames_city_index.cpp
    src/geonames_city_index.hpp
    src/geonames_library_reverse_geocoder.cpp
    src/geonames_library_reverse_geocoder.hpp
    src/justified_review_layout_model.cpp
    src/justified_review_layout_model.hpp
    src/review_gallery_grouping_controller.cpp
    src/review_gallery_grouping_controller.hpp
    src/library_server_controller_backend.cpp
    src/google_library_reverse_geocoder.cpp
    src/google_library_reverse_geocoder.hpp
    src/library_reverse_geocoder.hpp
    src/library_reverse_geocoder_router.cpp
    src/library_reverse_geocoder_router.hpp
    src/history_coordinator.cpp
    src/history_coordinator.hpp
    src/history_model.cpp
    src/history_model.hpp
    src/image_understanding_controller.cpp
    src/image_understanding_controller.hpp
    src/catalog_startup_recovery.cpp
    src/catalog_startup_recovery.hpp
    src/main.cpp
    src/pipeline_launch.cpp
    src/pipeline_launch.hpp
    src/paint_smoke_harness.cpp
    src/paint_smoke_harness.hpp
    src/precision_editing_smoke_harness.cpp
    src/precision_editing_smoke_harness.hpp
    src/rgb_curve_smoke_harness.cpp
    src/rgb_curve_smoke_harness.hpp
    src/pipeline_smoke_harness.cpp
    src/pipeline_smoke_harness.hpp
    src/pipeline_runtime.cpp
    src/pipeline_run_controller.cpp
    src/pipeline_run_controller.hpp
    src/ai_preferences.cpp
    src/ai_preferences.hpp
    src/cache_preferences.cpp
    src/cache_preferences.hpp
    src/personal_profile.cpp
    src/personal_profile.hpp
    src/personal_location_search.cpp
    src/personal_location_search.hpp
    src/people_analysis_controller.cpp
    src/people_analysis_controller.hpp
    src/semantic_search_controller.cpp
    src/semantic_search_controller.hpp
    src/smart_category_controller.cpp
    src/smart_category_controller.hpp
    src/lut_library.cpp
    src/lut_library.hpp
    src/optics_profile_library.cpp
    src/optics_profile_library.hpp
    src/photo_inspection_projection.cpp
    src/photo_inspection_projection.hpp
    src/preview_diagnostics.cpp
    src/preview_diagnostics.hpp
    src/lut_preview_provider.cpp
    src/lut_preview_provider.hpp
    src/map_provider_preferences.cpp
    src/map_provider_preferences.hpp
    src/map/library_web_map_controller.cpp
    src/map/library_web_map_controller.hpp
    src/review_comparison_coordinator.cpp
    src/review_comparison_coordinator.hpp
    src/review_controller.cpp
    src/review_controller.hpp
    src/review_controller_backend_operations.cpp
    src/review_controller_backend_operations.hpp
    src/review_controller_comparison.cpp
    src/review_controller_connections.cpp
    src/review_controller_decisions.cpp
    src/review_controller_inspection.cpp
    src/review_controller_library_management.cpp
    src/review_controller_library_keywords.cpp
    src/review_controller_library_map.cpp
    src/review_controller_location_completion.cpp
    src/review_controller_location_reference.cpp
    src/review_controller_library_metadata.cpp
    src/review_controller_library_query.cpp
    src/review_controller_remote_library.cpp
    src/review_decision_coordinator.cpp
    src/review_decision_coordinator.hpp
    src/review_decision_session.hpp
    src/review_evidence_session.hpp
    src/review_filter_model.cpp
    src/review_filter_model.hpp
    src/review_import_coordinator.cpp
    src/review_import_coordinator.hpp
    src/review_library_album_coordinator.cpp
    src/review_library_album_coordinator.hpp
    src/review_library_facet_coordinator.cpp
    src/review_library_facet_coordinator.hpp
    src/review_travel_collection_coordinator.cpp
    src/review_travel_collection_coordinator.hpp
    src/review_library_keyword_coordinator.cpp
    src/review_library_keyword_coordinator.hpp
    src/review_library_map_coordinator.cpp
    src/review_library_map_coordinator.hpp
    src/review_location_completion_coordinator.cpp
    src/review_location_completion_coordinator.hpp
    src/review_location_reference_coordinator.cpp
    src/review_location_reference_coordinator.hpp
    src/review_library_metadata_coordinator.cpp
    src/review_library_metadata_coordinator.hpp
    src/review_library_place_resolution_coordinator.cpp
    src/review_library_place_resolution_coordinator.hpp
    src/review_library_organization_coordinator.cpp
    src/review_library_organization_coordinator.hpp
    src/review_library_query_coordinator.cpp
    src/review_library_query_coordinator.hpp
    src/library_source_quick_probe.cpp
    src/library_source_quick_probe.hpp
    src/review_model.cpp
    src/review_model.hpp
    src/review_source_availability_monitor.cpp
    src/review_source_availability_monitor.hpp
    src/remote_library_connection_store.cpp
    src/remote_library_connection_store.hpp
    src/remote_photo_aggregation.cpp
    src/remote_photo_aggregation.hpp
    src/review_remote_library_coordinator.cpp
    src/review_remote_library_coordinator.hpp
    src/review_photo_inspection_coordinator.cpp
    src/review_photo_inspection_coordinator.hpp
    src/review_photo_inspection_session.hpp
    src/review_focus_detail_coordinator.cpp
    src/review_focus_detail_coordinator.hpp
    src/review_focus_detail_provider.cpp
    src/review_focus_detail_provider.hpp
    src/review_shared_grade_coordinator.cpp
    src/review_shared_grade_coordinator.hpp
    src/review_source_health_coordinator.cpp
    src/review_source_health_coordinator.hpp
    src/review_visual_request.cpp
    src/review_visual_request.hpp
    src/thumbnail_provider.cpp
    src/thumbnail_provider.hpp
    src/tone_curve_point_model.cpp
    src/tone_curve_point_model.hpp
    src/ui_preferences.cpp
    src/ui_preferences.hpp
)
set(
    SHADOW_GEONAMES_BUNDLE_INPUTS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/geonames/NOTICE.txt"
)
if(SHADOW_GEONAMES_CITY_INDEX_PATH)
    list(APPEND SHADOW_GEONAMES_BUNDLE_INPUTS "${SHADOW_GEONAMES_CITY_INDEX_PATH}")
endif()
set_property(
    TARGET shadow-desktop
    APPEND
    PROPERTY LINK_DEPENDS ${SHADOW_GEONAMES_BUNDLE_INPUTS}
)

qt_add_resources(
    shadow-desktop shadow-desktop-icons
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
    FILES
        icons/add-folder.svg
        icons/album-add.svg
        icons/album-remove.svg
        icons/back-to-library.svg
        icons/before-after.svg
        icons/brush.svg
        icons/candidate.svg
        icons/chevron-down.svg
        icons/check.svg
        icons/clear.svg
        icons/clone.svg
        icons/color-mixer-colors.svg
        icons/color-mixer-curves.svg
        icons/close.svg
        icons/compare.svg
        icons/compare-side-by-side.svg
        icons/compare-stacked.svg
        icons/compare-wipe-horizontal.svg
        icons/compare-wipe-vertical.svg
        icons/crop.svg
        icons/detail-loupe.svg
        icons/duplicate.svg
        icons/edit.svg
        icons/edit-off.svg
        icons/eraser.svg
        icons/eyedropper.svg
        icons/eyedropper-add.svg
        icons/eyedropper-subtract.svg
        icons/export.svg
        icons/fit-view.svg
        icons/flip-horizontal.svg
        icons/flip-vertical.svg
        icons/grade-node.svg
        icons/grade-node-add.svg
        icons/grade-node-paste.svg
        icons/filter.svg
        icons/filter-off.svg
        icons/filmstrip.svg
        icons/history.svg
        icons/heart.svg
        icons/heart-filled.svg
        icons/heal.svg
        icons/library-manage.svg
        icons/location-pin.svg
        icons/map.svg
        icons/mask.svg
        icons/mask-add.svg
        icons/mask-color-range.svg
        icons/mask-create.svg
        icons/mask-intersect.svg
        icons/mask-linear.svg
        icons/mask-luminance-range.svg
        icons/mask-radial.svg
        icons/mask-subtract.svg
        icons/metadata.svg
        icons/more-horizontal.svg
        icons/move-down.svg
        icons/move-up.svg
        icons/network.svg
        icons/node-add.svg
        icons/overlay-hide.svg
        icons/overlay-show.svg
        icons/pin.svg
        icons/pick.svg
        icons/people.svg
        icons/redo.svg
        icons/reject.svg
        icons/refresh.svg
        icons/reset-all.svg
        icons/retouch.svg
        icons/review-grid.svg
        icons/scopes.svg
        icons/settings.svg
        icons/shared-link.svg
        icons/source-available.svg
        icons/source-missing.svg
        icons/source-stack.svg
        icons/slot-left.svg
        icons/slot-right.svg
        icons/star.svg
        icons/star-filled.svg
        icons/storage.svg
        icons/swap.svg
        icons/tag.svg
        icons/tie.svg
        icons/trash.svg
        icons/unflag.svg
        icons/undo.svg
        icons/skip.svg
        icons/zebra.svg
        icons/zoom.svg
        icons/zoom-in.svg
        icons/zoom-out.svg
)

qt_add_resources(
    shadow-desktop shadow-desktop-assets
    PREFIX "/assets"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    FILES
        assets/lut-preview-reference.jpg
)

if(APPLE)
    set(SHADOW_APP_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/app-icon.icns")
    set_source_files_properties(
        "${SHADOW_APP_ICON_FILE}"
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources
    )
    target_sources(shadow-desktop PRIVATE "${SHADOW_APP_ICON_FILE}")

    target_link_libraries(
        shadow-desktop
        PRIVATE
            "-framework AppKit"
            "-framework CoreFoundation"
            "-framework Metal"
    )
endif()

set_source_files_properties(
    qml/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_executable(
    shadow-server-manager
    MACOSX_BUNDLE
    src/server_manager/main.cpp
)
qt_add_qml_module(
    shadow-server-manager
    URI Shadow.ServerManager
    VERSION 1.0
    QML_FILES
        qml/ShadowTextField.qml
        qml/ServerManagerMain.qml
        qml/SettingsLibraryServerPane.qml
        qml/ShadowButton.qml
        qml/ShadowSwitch.qml
        qml/Theme.qml
)
target_compile_features(shadow-server-manager PRIVATE cxx_std_20)
target_include_directories(
    shadow-server-manager
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
    shadow-server-manager
    PRIVATE
        shadow-library-server-control
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Widgets
)
add_dependencies(shadow-server-manager shadow-image-decode-helper)
add_custom_command(
    TARGET shadow-server-manager
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:shadow-image-decode-helper>
        $<TARGET_FILE_DIR:shadow-server-manager>/$<TARGET_FILE_NAME:shadow-image-decode-helper>
    COMMENT "Installing the isolated RAW decode helper beside Shadow Server"
)
if(MSVC)
    target_compile_options(shadow-server-manager PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        shadow-server-manager
        PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()
set_target_properties(
    shadow-server-manager
    PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Shadow Server"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.shadow.photo.server"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        OUTPUT_NAME "Shadow Server"
)

qt_add_qml_module(
    shadow-desktop
    URI Shadow.App
    VERSION 1.0
    QML_FILES
        qml/ReviewGalleryActionsMenu.qml
        qml/ShadowComboBox.qml
        qml/ShadowMenu.qml
        qml/ShadowMenuItem.qml
        qml/ShadowTextField.qml
        qml/EditHistogram.qml
        qml/EditScopeClipSummary.qml
        qml/EditScopeCanvas.qml
        qml/EditScopeData.qml
        qml/EditScopeToolbar.qml
        qml/ExportDialog.qml
        qml/ExportActivityButton.qml
        qml/ExportPresetMenus.qml
        qml/ExportPresetSidebar.qml
        qml/ExportSettingsPane.qml
        qml/ExportWatermarkPane.qml
        qml/HistoryDrawer.qml
        qml/LibraryAlbumCreateDialog.qml
        qml/LibraryAlbumDeleteDialog.qml
        qml/LibraryAlbumDialogs.qml
        qml/LibraryAlbumManageDialog.qml
        qml/LibraryAlbumMembershipDialog.qml
        qml/LibraryAdvancedFilterPopup.qml
        qml/LibraryPlaceResolutionStatus.qml
        qml/LibraryImportPane.qml
        qml/LibraryKeywordDialogs.qml
        qml/LibraryKeywordPanel.qml
        qml/LibraryKeywordPopup.qml
        qml/LibraryGpxImportDialog.qml
        qml/LibraryCaptureTimeBatchDialog.qml
        qml/LibraryLocationBatchDialog.qml
        qml/LibraryLocationBatchState.qml
        qml/LibraryLocationCompletionGallery.qml
        qml/LibraryMetadataEditor.qml
        qml/LibraryMapLocationPlacementState.qml
        qml/LibraryWebMapSurface.qml
        qml/LibraryMapPlaceSearch.qml
        qml/LibraryMapProviderOverlay.qml
        qml/LibraryMapView.qml
        qml/LibraryMapWorkspace.qml
        qml/LibraryRemoteConnectionsPane.qml
        qml/LibrarySourceHealthPane.qml
        qml/LibraryWorkspace.qml
        qml/LutManagerWindow.qml
        qml/LutExportDialog.qml
        qml/ApplicationSettingsDialog.qml
        qml/SettingsGeneralPane.qml
        qml/SettingsLibraryPane.qml
        qml/SettingsAiPane.qml
        qml/SettingsStoragePane.qml
        qml/Main.qml
        qml/PipelineEditor.qml
        qml/PipelineExportDialog.qml
        qml/MainTitleBar.qml
        qml/ShadowTitleBar.qml
        qml/MainLibraryFilterBar.qml
        qml/MainLibrarySortMenu.qml
        qml/MainPrecisionProxyStatus.qml
        qml/MainStatusBar.qml
        qml/MapProviderSettingsDialog.qml
        qml/MapProviderSettingsPane.qml
        qml/AutosaveFailureRecovery.qml
        qml/MetadataFieldSelectorRow.qml
        qml/MetadataWindow.qml
        qml/OpticsProfileManagerWindow.qml
        qml/PersonalProfileDialog.qml
        qml/PersonalLocationSearchField.qml
        qml/PersonalLivingPlacesEditor.qml
        qml/PeopleWorkspace.qml
        qml/PrecisionCanvas.qml
        qml/PrecisionViewportState.qml
        qml/PrecisionDetailSurface.qml
        qml/PrecisionDetailLoupe.qml
        qml/PrecisionDetailLoupeState.qml
        qml/PrecisionActiveStrokeCoverage.qml
        qml/PrecisionAiMaskPromptOverlay.qml
        qml/PrecisionPaintOverlay.qml
        qml/PrecisionPaintTools.qml
        qml/PrecisionPaintBrushSettings.qml
        qml/PrecisionAiCompletionOverlay.qml
        qml/PrecisionAiCompletionTools.qml
        qml/PrecisionPeopleMaskSelector.qml
        qml/PrecisionCanvasPickerInput.qml
        qml/PrecisionRetouchSourcePreview.qml
        qml/PrecisionCanvasStatusOverlays.qml
        qml/PrecisionCanvasToolbar.qml
        qml/PrecisionVariantSelector.qml
        qml/PrecisionCanvasZoomInput.qml
        qml/PrecisionCaptureMetadata.qml
        qml/PrecisionComparisonSurface.qml
        qml/PrecisionInspector.qml
        qml/PrecisionLiquifyOverlay.qml
        qml/PrecisionLiquifyTools.qml
        qml/PrecisionResetAllDialog.qml
        qml/PrecisionLutSection.qml
        qml/PrecisionPointColorSection.qml
        qml/PrecisionPointColorSampleBar.qml
        qml/PrecisionFoundationAdjustments.qml
        qml/PrecisionRawDenoiseAdjustments.qml
        qml/PrecisionColorMixer.qml
        qml/PrecisionSelectiveColor.qml
        qml/ColorWarperEditor.qml
        qml/HueCurveEditor.qml
        qml/PrecisionGeometryTools.qml
        qml/PrecisionGradeNodeMenus.qml
        qml/ShadowRecipeInterchangeDialog.qml
        qml/XmpImportDialog.qml
        qml/PrecisionGradeNodePane.qml
        qml/PrecisionLocalMaskOverlay.qml
        qml/PrecisionMaskCoverageOverlay.qml
        qml/PrecisionCropOverlay.qml
        qml/PrecisionRetouchOverlay.qml
        qml/PrecisionRetouchProjectedHandle.qml
        qml/PrecisionRetouchSpotHandle.qml
        qml/PrecisionRetouchStrokeCoverage.qml
        qml/PrecisionRetouchStrokeHandle.qml
        qml/PrecisionSubjectEmphasis.qml
        qml/PrecisionLocalMaskTools.qml
        qml/PrecisionConditionMaskTools.qml
        qml/PrecisionMaskCreateMenu.qml
        qml/PrecisionSemanticMaskPanel.qml
        qml/PrecisionRetouchTools.qml
        qml/PrecisionRetouchRegionInspector.qml
        qml/PrecisionRetouchRegionPicker.qml
        qml/PrecisionDetailSection.qml
        qml/PrecisionEffectsSection.qml
        qml/PrecisionOpticsSection.qml
        qml/PrecisionTechnicalTools.qml
        qml/PrecisionVersionsPane.qml
        qml/RecipeRecoveryPopup.qml
        qml/PrecisionWorkspace.qml
        qml/ReviewWorkspace.qml
        qml/ReviewSelectionState.qml
        qml/ReviewComparisonState.qml
        qml/ReviewComparisonView.qml
        qml/ReviewCullingState.qml
        qml/ReviewCullingArena.qml
        qml/ReviewDecisionToolbar.qml
        qml/ReviewGallerySurface.qml
        qml/ReviewGalleryToolbar.qml
        qml/ReviewGalleryGroupingControl.qml
        qml/ReviewGallerySectionNavigator.qml
        qml/SemanticSearchControl.qml
        qml/ReviewAlbumList.qml
        qml/ReviewComparisonEvidence.qml
        qml/ReviewImportProgressCard.qml
        qml/ReviewLibrarySidebar.qml
        qml/ReviewSmartCategoryList.qml
        qml/ReviewSmartCategorySettingsDialog.qml
        qml/ReviewSmartCategoryFeedbackPopup.qml
        qml/ReviewMetadataPresentation.qml
        qml/ReviewPhotoAffinity.qml
        qml/ReviewRemoteSourceIndicator.qml
        qml/ReviewSystemCollections.qml
        qml/ReviewDailyCollection.qml
        qml/ReviewTravelCollections.qml
        qml/ReviewSharedGradePicker.qml
        qml/ReviewPhotoCard.qml
        qml/LibraryMissingPhotoDialogs.qml
        qml/ReviewPhotoContextMenu.qml
        qml/ReviewExifSection.qml
        qml/ReviewPhotoInspector.qml
        qml/ReviewSelectionInspectionPane.qml
        qml/ReviewPhotoSummary.qml
        qml/ReviewSinglePreview.qml
        qml/ReviewPreviewViewport.qml
        qml/ShadowButton.qml
        qml/ShadowCheckBox.qml
        qml/ShadowColorLabelButton.qml
        qml/ShadowColorWheel.qml
        qml/ShadowAdjustmentSection.qml
        qml/ShadowAvatarButton.qml
        qml/ShadowHueRange.qml
        qml/ShadowIcon.qml
        qml/ShadowIconButton.qml
        qml/ShadowInlineSlider.qml
        qml/ShadowRoundedImage.qml
        qml/ShadowSlider.qml
        qml/ShadowSwitch.qml
        qml/ShadowSubsectionLabel.qml
        qml/ShadowTabButton.qml
        qml/Theme.qml
        qml/PhotoCompositionDialog.qml
        qml/ToneCurveEditor.qml
)

qt_add_translations(
    TARGETS shadow-desktop shadow-server-manager
    SOURCE_TARGETS shadow-desktop shadow-server-manager shadow-library-server-control
    TS_FILE_BASE shadow
    TS_FILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/i18n"
    RESOURCE_PREFIX "/i18n"
    LUPDATE_TARGET shadow-desktop-update-translations
    LUPDATE_OPTIONS -no-obsolete
    LRELEASE_TARGET shadow-desktop-release-translations
    LRELEASE_OPTIONS -nounfinished
    NO_GENERATE_PLURALS_TS_FILE
)

target_compile_features(shadow-desktop PRIVATE cxx_std_20)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    file(TO_CMAKE_PATH
        "${PROJECT_SOURCE_DIR}/local-reference/sample-assets/raw"
        SHADOW_DESKTOP_DEV_SAMPLE_FOLDER
    )
    target_compile_definitions(
        shadow-desktop
        PRIVATE
            SHADOW_DESKTOP_DEV_SAMPLE_FOLDER="${SHADOW_DESKTOP_DEV_SAMPLE_FOLDER}"
    )
endif()
target_include_directories(
    shadow-desktop
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/map"
        "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
        "${PROJECT_SOURCE_DIR}/cpp/shadow-image/include"
)
target_link_libraries(
    shadow-desktop
    PRIVATE
        shadow-library-server-control
        shadow-desktop-export-backend
        shadow-desktop-rust
        Shadow::Image
        PkgConfig::LibRaw
        PkgConfig::LittleCms
        ${CMAKE_DL_LIBS}
        JPEG::JPEG
        Iconv::Iconv
        Qt6::Concurrent
        Qt6::Network
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Svg
        Qt6::WebView
        Qt6::Widgets
)
add_dependencies(shadow-desktop shadow-image-decode-helper)
add_dependencies(shadow-desktop shadow-server-manager)
add_custom_command(
    TARGET shadow-desktop
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:shadow-image-decode-helper>
        $<TARGET_FILE_DIR:shadow-desktop>/$<TARGET_FILE_NAME:shadow-image-decode-helper>
    COMMENT "Installing the isolated RAW decode helper beside Shadow"
)
if(APPLE)
    add_custom_command(
        TARGET shadow-desktop
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E remove_directory
            "$<TARGET_BUNDLE_DIR:shadow-desktop>/Contents/Applications/Shadow Server.app"
        COMMAND
            ${CMAKE_COMMAND} -E copy_directory
            "$<TARGET_BUNDLE_DIR:shadow-server-manager>"
            "$<TARGET_BUNDLE_DIR:shadow-desktop>/Contents/Applications/Shadow Server.app"
        COMMENT "Installing the standalone Shadow Server controller inside Shadow"
        VERBATIM
    )
endif()
add_custom_command(
    TARGET shadow-desktop
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND} -E make_directory
        "${SHADOW_GEONAMES_BUNDLE_DIRECTORY}"
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/geonames/NOTICE.txt"
        "${SHADOW_GEONAMES_BUNDLE_DIRECTORY}/NOTICE.txt"
    COMMENT "Installing the GeoNames offline-location attribution"
    VERBATIM
)
if(SHADOW_GEONAMES_CITY_INDEX_PATH)
    add_custom_command(
        TARGET shadow-desktop
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${SHADOW_GEONAMES_CITY_INDEX_PATH}"
            "${SHADOW_GEONAMES_BUNDLE_DIRECTORY}/shadow-geonames-cities-v1.tsv"
        COMMENT "Installing the GeoNames offline city index"
        VERBATIM
    )
else()
    add_custom_command(
        TARGET shadow-desktop
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E rm -f
            "${SHADOW_GEONAMES_BUNDLE_DIRECTORY}/shadow-geonames-cities-v1.tsv"
        COMMENT "Ensuring no stale GeoNames city index remains in this build"
        VERBATIM
    )
endif()
if(Lensfun_FOUND)
    target_link_libraries(shadow-desktop PRIVATE PkgConfig::Lensfun)
endif()
if(LibHeif_FOUND)
    # `shadow-desktop-rust` is a static Rust archive that compiles the same native router as the
    # CMake image library. Propagate libheif here as well: Cargo's link metadata is consumed by
    # Rust binaries, not by this Qt/CMake final link step.
    target_link_libraries(shadow-desktop PRIVATE PkgConfig::LibHeif)
endif()

if(MSVC)
    target_compile_options(shadow-desktop PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        shadow-desktop
        PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()

set_target_properties(
    shadow-desktop
    PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Shadow"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.shadow.photo"
        MACOSX_BUNDLE_ICON_FILE "app-icon.icns"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "2"
        OUTPUT_NAME "Shadow"
)

include(cmake/ShadowDesktopTests.cmake)

include(cmake/ShadowDesktopComposition.cmake)

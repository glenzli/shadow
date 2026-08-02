find_package(
    Qt6 6.11.1 REQUIRED
    COMPONENTS Core Concurrent LinguistTools Location Network Positioning Quick QuickControls2 Svg Widgets
)
find_package(PkgConfig REQUIRED)
find_package(JPEG REQUIRED)
find_package(Iconv REQUIRED)
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

set(
    SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR
    ""
    CACHE PATH
    "Optional frozen RawNIND onedir provider copied beside the Shadow executable"
)
set(
    SHADOW_RAWNIND_FOUNDATION_PROVIDER_NAME
    "shadow-rawnind-foundation-provider"
)
set(
    SHADOW_RAWNIND_FOUNDATION_RUNTIME_DIRECTORY
    "_rawnind_runtime"
)
if(APPLE)
    set(
        SHADOW_RAWNIND_FOUNDATION_BUNDLE_DIRECTORY
        "$<TARGET_BUNDLE_DIR:shadow-desktop>/Contents/Helpers/RawNIND"
    )
else()
    set(
        SHADOW_RAWNIND_FOUNDATION_BUNDLE_DIRECTORY
        "$<TARGET_FILE_DIR:shadow-desktop>"
    )
endif()
if(SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR)
    if(NOT IS_DIRECTORY "${SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR}")
        message(FATAL_ERROR
            "SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR is not a directory: "
            "${SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR}"
        )
    endif()
    set(
        SHADOW_RAWNIND_FOUNDATION_PROVIDER_EXECUTABLE
        "${SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR}/${SHADOW_RAWNIND_FOUNDATION_PROVIDER_NAME}"
    )
    set(
        SHADOW_RAWNIND_FOUNDATION_PROVIDER_RUNTIME
        "${SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR}/${SHADOW_RAWNIND_FOUNDATION_RUNTIME_DIRECTORY}"
    )
    if(NOT EXISTS "${SHADOW_RAWNIND_FOUNDATION_PROVIDER_EXECUTABLE}")
        message(FATAL_ERROR
            "frozen RawNIND provider executable is missing: "
            "${SHADOW_RAWNIND_FOUNDATION_PROVIDER_EXECUTABLE}"
        )
    endif()
    if(NOT IS_DIRECTORY "${SHADOW_RAWNIND_FOUNDATION_PROVIDER_RUNTIME}")
        message(FATAL_ERROR
            "frozen RawNIND provider runtime is missing: "
            "${SHADOW_RAWNIND_FOUNDATION_PROVIDER_RUNTIME}"
        )
    endif()
    message(STATUS
        "Shadow will bundle frozen RawNIND provider from "
        "${SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR}"
    )
endif()
set(
    SHADOW_RAWNIND_FOUNDATION_BUNDLE_CONFIGURATION
    "${CMAKE_CURRENT_BINARY_DIR}/raw-foundation-provider-configuration.txt"
)
set(
    SHADOW_RAWNIND_FOUNDATION_BUNDLE_CONFIGURATION_CONTENT
    "provider_dir=${SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR}\nprovider_name=${SHADOW_RAWNIND_FOUNDATION_PROVIDER_NAME}\nruntime_directory=${SHADOW_RAWNIND_FOUNDATION_RUNTIME_DIRECTORY}\n"
)
file(
    GENERATE
    OUTPUT "${SHADOW_RAWNIND_FOUNDATION_BUNDLE_CONFIGURATION}"
    CONTENT "${SHADOW_RAWNIND_FOUNDATION_BUNDLE_CONFIGURATION_CONTENT}"
)

if(APPLE)
    add_subdirectory(
        providers/sam2-coreml
        "${CMAKE_CURRENT_BINARY_DIR}/providers/sam2-coreml"
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
    "${SHADOW_CARGO_TARGET_DIRECTORY}/${SHADOW_CARGO_PROFILE}/libshadow_desktop_bridge.a"
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

if(APPLE AND SHADOW_ENABLE_METAL)
    set(
        SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE
        src/edit_preview_metal_texture_factory.mm
    )
else()
    set(
        SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE
        src/edit_preview_metal_texture_factory_stub.cpp
    )
endif()

qt_add_executable(
    shadow-desktop
    MACOSX_BUNDLE
    ${SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES}
    ${SHADOW_DESKTOP_HISTORY_PROJECTION_SOURCES}
    src/desktop_backend.cpp
    src/desktop_backend_ai_mask.cpp
    src/desktop_backend_raw_foundation.cpp
    src/desktop_backend_cache.cpp
    src/desktop_backend_edit.cpp
    src/desktop_backend_history.cpp
    src/desktop_backend_library.cpp
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
    src/backend/library_types.hpp
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
    src/edit_adjustment_controller.cpp
    src/edit_adjustment_reset_controller.cpp
    src/edit_ai_mask_controller.cpp
    src/edit_ai_mask_controller.hpp
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
    src/edit_liquify_coordinates.cpp
    src/edit_liquify_coordinates.hpp
    src/edit_liquify_controller.cpp
    src/edit_mask_coverage_contract.hpp
    src/edit_mask_coverage_controller.cpp
    src/edit_mask_coverage_store.cpp
    src/edit_mask_assignment_controller.cpp
    src/edit_optics_controller.cpp
    src/edit_persistence_coordinator.cpp
    src/edit_render_coordinator.cpp
    src/edit_retouch_controller.cpp
    src/edit_stroke_input.cpp
    src/edit_stroke_input.hpp
    src/edit_task_runner.cpp
    src/edit_task_runner.hpp
    src/edit_tone_curve_controller.cpp
    src/edit_history.hpp
    src/edit_preview_contract.hpp
    src/edit_preview_liquify_mesh.cpp
    src/edit_preview_liquify_mesh.hpp
    src/edit_preview_metal_texture_factory.hpp
    ${SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE}
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
    src/export_controller.cpp
    src/export_controller.hpp
    src/export_preset_store.cpp
    src/export_preset_store.hpp
    src/export_task_runner.cpp
    src/export_task_runner.hpp
    src/default_library_reverse_geocoder.cpp
    src/default_library_reverse_geocoder.hpp
    src/geonames_city_index.cpp
    src/geonames_city_index.hpp
    src/geonames_library_reverse_geocoder.cpp
    src/geonames_library_reverse_geocoder.hpp
    src/justified_review_layout_model.cpp
    src/justified_review_layout_model.hpp
    src/google_library_reverse_geocoder.cpp
    src/google_library_reverse_geocoder.hpp
    src/library_reverse_geocoder.hpp
    src/library_reverse_geocoder_router.cpp
    src/library_reverse_geocoder_router.hpp
    src/history_coordinator.cpp
    src/history_coordinator.hpp
    src/history_model.cpp
    src/history_model.hpp
    src/main.cpp
    src/ai_preferences.cpp
    src/ai_preferences.hpp
    src/cache_preferences.cpp
    src/cache_preferences.hpp
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
    src/map/google_map_tile_geometry.cpp
    src/map/google_map_tile_geometry.hpp
    src/map/google_map_tile_layer.cpp
    src/map/google_map_tile_layer.hpp
    src/map/google_map_tiles_protocol.cpp
    src/map/google_map_tiles_protocol.hpp
    src/map/google_map_tiles_service.cpp
    src/map/google_map_tiles_service.hpp
    src/secure_secret_store.cpp
    src/secure_secret_store.hpp
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
    src/review_controller_library_metadata.cpp
    src/review_controller_library_query.cpp
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
    src/review_library_keyword_coordinator.cpp
    src/review_library_keyword_coordinator.hpp
    src/review_library_map_coordinator.cpp
    src/review_library_map_coordinator.hpp
    src/review_library_metadata_coordinator.cpp
    src/review_library_metadata_coordinator.hpp
    src/review_library_place_resolution_coordinator.cpp
    src/review_library_place_resolution_coordinator.hpp
    src/review_library_organization_coordinator.cpp
    src/review_library_organization_coordinator.hpp
    src/review_library_query_coordinator.cpp
    src/review_library_query_coordinator.hpp
    src/review_model.cpp
    src/review_model.hpp
    src/review_photo_inspection_coordinator.cpp
    src/review_photo_inspection_coordinator.hpp
    src/review_photo_inspection_session.hpp
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
    SHADOW_RAWNIND_FOUNDATION_BUNDLE_INPUTS
    "${SHADOW_RAWNIND_FOUNDATION_BUNDLE_CONFIGURATION}"
    "${CMAKE_CURRENT_SOURCE_DIR}/providers/rawnind-foundation/install_runtime.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/providers/rawnind-foundation/model-manifest.json"
)
if(SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR)
    file(
        GLOB_RECURSE SHADOW_RAWNIND_FOUNDATION_PROVIDER_INPUTS
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES FALSE
        "${SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR}/*"
    )
    list(
        APPEND SHADOW_RAWNIND_FOUNDATION_BUNDLE_INPUTS
        ${SHADOW_RAWNIND_FOUNDATION_PROVIDER_INPUTS}
    )
endif()
set_property(
    TARGET shadow-desktop
    APPEND
    PROPERTY LINK_DEPENDS ${SHADOW_RAWNIND_FOUNDATION_BUNDLE_INPUTS}
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
        icons/before-after.svg
        icons/brush.svg
        icons/chevron-down.svg
        icons/check.svg
        icons/clear.svg
        icons/clone.svg
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
        icons/filter.svg
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
        icons/move-down.svg
        icons/move-up.svg
        icons/node-add.svg
        icons/overlay-hide.svg
        icons/overlay-show.svg
        icons/pin.svg
        icons/pick.svg
        icons/redo.svg
        icons/reject.svg
        icons/reset-all.svg
        icons/retouch.svg
        icons/review-grid.svg
        icons/scopes.svg
        icons/settings.svg
        icons/shared-link.svg
        icons/slot-left.svg
        icons/slot-right.svg
        icons/star.svg
        icons/star-filled.svg
        icons/tag.svg
        icons/trash.svg
        icons/unflag.svg
        icons/undo.svg
        icons/zebra.svg
        icons/zoom.svg
)

qt_add_resources(
    shadow-desktop shadow-desktop-assets
    PREFIX "/assets"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    FILES
        assets/lut-preview-reference.jpg
)

if(APPLE)
    target_sources(
        shadow-desktop
        PRIVATE
            src/mac_titlebar.hpp
            src/mac_titlebar.mm
            src/secure_secret_store_mac.mm
    )
    target_link_libraries(
        shadow-desktop
        PRIVATE
            "-framework AppKit"
            "-framework CoreFoundation"
            "-framework Metal"
            "-framework Security"
    )
else()
    target_sources(
        shadow-desktop
        PRIVATE src/secure_secret_store_stub.cpp
    )
endif()

set_source_files_properties(
    qml/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(
    shadow-desktop
    URI Shadow.App
    VERSION 1.0
    QML_FILES
        qml/EditHistogram.qml
        qml/EditScopeClipSummary.qml
        qml/EditScopeCanvas.qml
        qml/EditScopeData.qml
        qml/EditScopeToolbar.qml
        qml/ExportDialog.qml
        qml/ExportPresetMenus.qml
        qml/ExportSettingsPane.qml
        qml/HistoryDrawer.qml
        qml/LibraryAlbumCreateDialog.qml
        qml/LibraryAlbumDeleteDialog.qml
        qml/LibraryAlbumDialogs.qml
        qml/LibraryAlbumManageDialog.qml
        qml/LibraryAlbumMembershipDialog.qml
        qml/LibraryAdvancedFilterPopup.qml
        qml/LibraryFacetPopup.qml
        qml/LibraryPlaceResolutionStatus.qml
        qml/LibraryImportPane.qml
        qml/LibraryKeywordDialogs.qml
        qml/LibraryKeywordPanel.qml
        qml/LibraryKeywordPopup.qml
        qml/LibraryGpxImportDialog.qml
        qml/LibraryCaptureTimeBatchDialog.qml
        qml/LibraryMetadataEditor.qml
        qml/LibraryMapLocationPlacementState.qml
        qml/LibraryMapProviderOverlay.qml
        qml/LibraryMapView.qml
        qml/LibrarySourceHealthPane.qml
        qml/LibraryWorkspace.qml
        qml/LutManagerWindow.qml
        qml/ApplicationSettingsDialog.qml
        qml/SettingsGeneralPane.qml
        qml/SettingsLibraryPane.qml
        qml/SettingsAiPane.qml
        qml/SettingsStoragePane.qml
        qml/Main.qml
        qml/MainTitleBar.qml
        qml/MainLibraryFilterBar.qml
        qml/MainLibrarySortMenu.qml
        qml/MainPrecisionProxyStatus.qml
        qml/MainStatusBar.qml
        qml/MapProviderSettingsDialog.qml
        qml/MapProviderSettingsPane.qml
        qml/AutosaveFailureRecovery.qml
        qml/MetadataWindow.qml
        qml/OpticsProfileManagerWindow.qml
        qml/PrecisionCanvas.qml
        qml/PrecisionDetailLoupe.qml
        qml/PrecisionDetailLoupeState.qml
        qml/PrecisionActiveStrokeCoverage.qml
        qml/PrecisionAiMaskPromptOverlay.qml
        qml/PrecisionCanvasPickerInput.qml
        qml/PrecisionCanvasStatusOverlays.qml
        qml/PrecisionCanvasToolbar.qml
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
        qml/PrecisionGradeNodePane.qml
        qml/PrecisionLocalMaskOverlay.qml
        qml/PrecisionMaskCoverageOverlay.qml
        qml/PrecisionCropOverlay.qml
        qml/PrecisionRetouchOverlay.qml
        qml/PrecisionRetouchSpotHandle.qml
        qml/PrecisionRetouchStrokeCoverage.qml
        qml/PrecisionRetouchStrokeHandle.qml
        qml/PrecisionLocalMaskTools.qml
        qml/PrecisionMaskCreateMenu.qml
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
        qml/ReviewDecisionToolbar.qml
        qml/ReviewGallerySurface.qml
        qml/ReviewGalleryToolbar.qml
        qml/ReviewAlbumList.qml
        qml/ReviewComparisonEvidence.qml
        qml/ReviewImportProgressCard.qml
        qml/ReviewLibrarySidebar.qml
        qml/ReviewMetadataPresentation.qml
        qml/ReviewPhotoAffinity.qml
        qml/ReviewSystemCollections.qml
        qml/ReviewSharedGradePicker.qml
        qml/ReviewPhotoCard.qml
        qml/LibraryMissingPhotoDialogs.qml
        qml/ReviewComparisonSlots.qml
        qml/ReviewPhotoContextMenu.qml
        qml/ReviewExifSection.qml
        qml/ReviewPhotoInspector.qml
        qml/ReviewPhotoSummary.qml
        qml/ReviewSinglePreview.qml
        qml/ShadowButton.qml
        qml/ShadowCheckBox.qml
        qml/ShadowColorLabelButton.qml
        qml/ShadowColorWheel.qml
        qml/ShadowAdjustmentSection.qml
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
        qml/ToneCurveEditor.qml
)

qt_add_translations(
    TARGETS shadow-desktop
    SOURCE_TARGETS shadow-desktop
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
        shadow-desktop-export-backend
        shadow-desktop-rust
        Shadow::Image
        PkgConfig::LibRaw
        PkgConfig::LittleCms
        ${CMAKE_DL_LIBS}
        JPEG::JPEG
        Iconv::Iconv
        Qt6::Concurrent
        Qt6::Location
        Qt6::Network
        Qt6::Positioning
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Svg
        Qt6::Widgets
)
add_dependencies(shadow-desktop shadow-image-decode-helper)
add_custom_command(
    TARGET shadow-desktop
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:shadow-image-decode-helper>
        $<TARGET_FILE_DIR:shadow-desktop>/$<TARGET_FILE_NAME:shadow-image-decode-helper>
    COMMENT "Installing the isolated RAW decode helper beside Shadow"
)
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
add_custom_command(
    TARGET shadow-desktop
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND} -E make_directory
        "${SHADOW_RAWNIND_FOUNDATION_BUNDLE_DIRECTORY}"
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/providers/rawnind-foundation/model-manifest.json"
        "${SHADOW_RAWNIND_FOUNDATION_BUNDLE_DIRECTORY}/shadow-rawnind-foundation-model-manifest.json"
    COMMENT "Installing the canonical RawNIND model manifest for Shadow"
    VERBATIM
)
if(SHADOW_RAWNIND_FOUNDATION_PROVIDER_DIR)
    add_custom_command(
        TARGET shadow-desktop
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${SHADOW_RAWNIND_FOUNDATION_PROVIDER_EXECUTABLE}"
            "${SHADOW_RAWNIND_FOUNDATION_BUNDLE_DIRECTORY}/${SHADOW_RAWNIND_FOUNDATION_PROVIDER_NAME}"
        COMMAND
            ${CMAKE_COMMAND}
            "-DSHADOW_RAWNIND_SOURCE_RUNTIME=${SHADOW_RAWNIND_FOUNDATION_PROVIDER_RUNTIME}"
            "-DSHADOW_RAWNIND_DESTINATION_RUNTIME=${SHADOW_RAWNIND_FOUNDATION_BUNDLE_DIRECTORY}/${SHADOW_RAWNIND_FOUNDATION_RUNTIME_DIRECTORY}"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/providers/rawnind-foundation/install_runtime.cmake"
        COMMENT "Installing the self-contained RawNIND provider beside Shadow"
        VERBATIM
    )
else()
    add_custom_command(
        TARGET shadow-desktop
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E rm -f
            "${SHADOW_RAWNIND_FOUNDATION_BUNDLE_DIRECTORY}/${SHADOW_RAWNIND_FOUNDATION_PROVIDER_NAME}"
        COMMAND
            ${CMAKE_COMMAND} -E remove_directory
            "${SHADOW_RAWNIND_FOUNDATION_BUNDLE_DIRECTORY}/${SHADOW_RAWNIND_FOUNDATION_RUNTIME_DIRECTORY}"
        COMMENT "Ensuring the optional RawNIND provider is absent from this Shadow build"
        VERBATIM
    )
endif()
if(APPLE)
    add_custom_command(
        TARGET shadow-desktop
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:shadow-desktop>/${SHADOW_RAWNIND_FOUNDATION_PROVIDER_NAME}"
        COMMAND
            ${CMAKE_COMMAND} -E rm -f
            "$<TARGET_FILE_DIR:shadow-desktop>/shadow-rawnind-foundation-model-manifest.json"
        COMMAND
            ${CMAKE_COMMAND} -E remove_directory
            "$<TARGET_FILE_DIR:shadow-desktop>/${SHADOW_RAWNIND_FOUNDATION_RUNTIME_DIRECTORY}"
        COMMENT "Removing the obsolete RawNIND Contents/MacOS layout"
        VERBATIM
    )
endif()
if(APPLE)
    add_dependencies(shadow-desktop shadow-sam2-coreml-provider)
    add_custom_command(
        TARGET shadow-desktop
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:shadow-sam2-coreml-provider>
            $<TARGET_FILE_DIR:shadow-desktop>/$<TARGET_FILE_NAME:shadow-sam2-coreml-provider>
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/providers/sam2-coreml/model-manifest.json"
            "$<TARGET_FILE_DIR:shadow-desktop>/shadow-sam2-coreml-model-manifest.json"
        COMMENT "Installing the local SAM 2.1 Core ML provider beside Shadow"
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
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        OUTPUT_NAME "Shadow"
)

if(BUILD_TESTING)
    add_test(
        NAME shadow-desktop-qml-startup
        COMMAND $<TARGET_FILE:shadow-desktop>
    )

    add_executable(
        shadow-backend-history-projection-test
        tests/backend_history_projection_test.cpp
        ${SHADOW_DESKTOP_HISTORY_PROJECTION_SOURCES}
    )
    target_compile_features(
        shadow-backend-history-projection-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-backend-history-projection-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-backend-history-projection-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-backend-history-projection-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-backend-history-projection-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-backend-history-projection-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-backend-history-projection
        COMMAND shadow-backend-history-projection-test
    )

    add_executable(
        shadow-history-coordinator-test
        tests/history_coordinator_test.cpp
        src/history_coordinator.cpp
        src/history_coordinator.hpp
        src/history_model.cpp
        src/history_model.hpp
    )
    target_compile_features(
        shadow-history-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-history-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-history-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-history-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-history-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-history-coordinator
        COMMAND shadow-history-coordinator-test
    )

    add_executable(
        shadow-history-drawer-contract-test
        tests/history_drawer_contract_test.cpp
    )
    target_compile_features(
        shadow-history-drawer-contract-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-history-drawer-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-history-drawer-contract-test
        URI Shadow.HistoryDrawerContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/HistoryDrawer.qml
            qml/ShadowButton.qml
            qml/ShadowIcon.qml
            qml/ShadowIconButton.qml
            qml/ShadowTabButton.qml
            qml/Theme.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-history-drawer-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-history-drawer-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-history-drawer-contract
        COMMAND shadow-history-drawer-contract-test
    )
    set_tests_properties(
        shadow-desktop-history-drawer-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    set_tests_properties(
        shadow-desktop-qml-startup
        PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;SHADOW_DESKTOP_SMOKE_TEST=1;SHADOW_DESKTOP_DATA_ROOT=${CMAKE_CURRENT_BINARY_DIR}/qml-startup-smoke"
    )

    add_executable(
        shadow-edit-fine-parameter-registry-test
        tests/edit_fine_parameter_registry_test.cpp
        src/edit_fine_parameter_registry.cpp
    )
    target_compile_features(
        shadow-edit-fine-parameter-registry-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-fine-parameter-registry-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-fine-parameter-registry-test
        PRIVATE Qt6::Core Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-fine-parameter-registry-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-fine-parameter-registry-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-fine-parameter-registry
        COMMAND shadow-edit-fine-parameter-registry-test
    )

    add_executable(
        shadow-backend-export-contract-test
        tests/backend_export_contract_test.cpp
    )
    target_compile_features(
        shadow-backend-export-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-backend-export-contract-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-backend-export-contract-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-backend-export-contract-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-backend-export-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-backend-export-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-backend-export-contract
        COMMAND shadow-backend-export-contract-test
    )

    add_executable(
        shadow-export-preset-store-test
        tests/export_preset_store_test.cpp
        src/backend/export_settings_codec.cpp
        src/backend/export_settings_codec.hpp
        src/export_preset_store.cpp
        src/export_preset_store.hpp
    )
    target_compile_features(
        shadow-export-preset-store-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-export-preset-store-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-export-preset-store-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-export-preset-store-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-export-preset-store-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-export-preset-store
        COMMAND shadow-export-preset-store-test
    )

    add_executable(
        shadow-folder-scan-backend-contract-test
        tests/folder_scan_backend_contract_test.cpp
        src/folder_scan_backend.cpp
        src/folder_scan_backend.hpp
    )
    target_compile_features(
        shadow-folder-scan-backend-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-folder-scan-backend-contract-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-folder-scan-backend-contract-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-folder-scan-backend-contract-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-folder-scan-backend-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-folder-scan-backend-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-folder-scan-backend-contract
        COMMAND shadow-folder-scan-backend-contract-test
    )

    add_executable(
        shadow-backend-photo-inspection-contract-test
        tests/backend_photo_inspection_contract_test.cpp
        ${SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES}
        src/desktop_backend.cpp
        src/desktop_backend.hpp
        src/folder_scan_backend.cpp
        src/folder_scan_backend.hpp
        src/photo_inspection_projection.cpp
        src/photo_inspection_projection.hpp
        src/preview_diagnostics.cpp
        src/preview_diagnostics.hpp
    )
    target_compile_features(
        shadow-backend-photo-inspection-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-backend-photo-inspection-contract-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-backend-photo-inspection-contract-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-backend-photo-inspection-contract-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-backend-photo-inspection-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-backend-photo-inspection-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-backend-photo-inspection-contract
        COMMAND shadow-backend-photo-inspection-contract-test
    )

    add_executable(
        shadow-review-source-health-backend-contract-test
        tests/review_source_health_backend_contract_test.cpp
        ${SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES}
        src/desktop_backend.cpp
        src/desktop_backend_library.cpp
        src/desktop_backend.hpp
        src/folder_scan_backend.cpp
        src/folder_scan_backend.hpp
        src/photo_inspection_projection.cpp
        src/photo_inspection_projection.hpp
        src/preview_diagnostics.cpp
        src/preview_diagnostics.hpp
    )
    target_compile_features(
        shadow-review-source-health-backend-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-source-health-backend-contract-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-review-source-health-backend-contract-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-review-source-health-backend-contract-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-review-source-health-backend-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-source-health-backend-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-source-health-backend-contract
        COMMAND shadow-review-source-health-backend-contract-test
    )

    add_executable(
        shadow-ui-preferences-test
        tests/ui_preferences_test.cpp
        src/ui_preferences.cpp
        src/ui_preferences.hpp
    )
    target_compile_features(shadow-ui-preferences-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-ui-preferences-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-ui-preferences-test
        PRIVATE Qt6::Gui Qt6::Qml
    )
    if(MSVC)
        target_compile_options(shadow-ui-preferences-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-ui-preferences-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-ui-preferences COMMAND shadow-ui-preferences-test)

    add_executable(
        shadow-ai-preferences-test
        tests/ai_preferences_test.cpp
        src/ai_preferences.cpp
        src/ai_preferences.hpp
    )
    target_compile_features(shadow-ai-preferences-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-ai-preferences-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-ai-preferences-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(shadow-ai-preferences-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-ai-preferences-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-ai-preferences COMMAND shadow-ai-preferences-test)

    add_executable(
        shadow-cache-preferences-test
        tests/cache_preferences_test.cpp
        src/cache_preferences.cpp
        src/cache_preferences.hpp
    )
    target_compile_features(shadow-cache-preferences-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-cache-preferences-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-cache-preferences-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(shadow-cache-preferences-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-cache-preferences-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-cache-preferences COMMAND shadow-cache-preferences-test)

    add_executable(
        shadow-map-provider-preferences-test
        tests/map_provider_preferences_test.cpp
        src/map_provider_preferences.cpp
        src/map_provider_preferences.hpp
        src/secure_secret_store.hpp
    )
    target_compile_features(
        shadow-map-provider-preferences-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-map-provider-preferences-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-map-provider-preferences-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-map-provider-preferences-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-map-provider-preferences-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-map-provider-preferences
        COMMAND shadow-map-provider-preferences-test
    )

    add_executable(
        shadow-google-map-tiles-protocol-test
        tests/google_map_tiles_protocol_test.cpp
        src/map/google_map_tiles_protocol.cpp
        src/map/google_map_tiles_protocol.hpp
    )
    target_compile_features(
        shadow-google-map-tiles-protocol-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-google-map-tiles-protocol-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-google-map-tiles-protocol-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-google-map-tiles-protocol-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-google-map-tiles-protocol-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-google-map-tiles-protocol
        COMMAND shadow-google-map-tiles-protocol-test
    )

    add_executable(
        shadow-google-map-tile-geometry-test
        tests/google_map_tile_geometry_test.cpp
        src/map/google_map_tile_geometry.cpp
        src/map/google_map_tile_geometry.hpp
        src/map/google_map_tiles_protocol.cpp
        src/map/google_map_tiles_protocol.hpp
    )
    target_compile_features(
        shadow-google-map-tile-geometry-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-google-map-tile-geometry-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-google-map-tile-geometry-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-google-map-tile-geometry-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-google-map-tile-geometry-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-google-map-tile-geometry
        COMMAND shadow-google-map-tile-geometry-test
    )

    add_executable(
        shadow-google-map-tiles-service-test
        tests/google_map_tiles_service_test.cpp
        src/map_provider_preferences.cpp
        src/map_provider_preferences.hpp
        src/secure_secret_store.hpp
        src/map/google_map_tile_geometry.cpp
        src/map/google_map_tile_geometry.hpp
        src/map/google_map_tile_layer.cpp
        src/map/google_map_tile_layer.hpp
        src/map/google_map_tiles_protocol.cpp
        src/map/google_map_tiles_protocol.hpp
        src/map/google_map_tiles_service.cpp
        src/map/google_map_tiles_service.hpp
    )
    target_compile_features(
        shadow-google-map-tiles-service-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-google-map-tiles-service-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-google-map-tiles-service-test
        PRIVATE Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-google-map-tiles-service-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-google-map-tiles-service-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-google-map-tiles-service
        COMMAND shadow-google-map-tiles-service-test
    )
    set_tests_properties(
        shadow-desktop-google-map-tiles-service
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-google-library-reverse-geocoder-test
        tests/google_library_reverse_geocoder_test.cpp
        src/google_library_reverse_geocoder.cpp
        src/google_library_reverse_geocoder.hpp
        src/library_reverse_geocoder.hpp
        src/map_provider_preferences.cpp
        src/map_provider_preferences.hpp
        src/secure_secret_store.hpp
    )
    target_compile_features(
        shadow-google-library-reverse-geocoder-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-google-library-reverse-geocoder-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-google-library-reverse-geocoder-test
        PRIVATE Qt6::Core Qt6::Network
    )
    if(MSVC)
        target_compile_options(
            shadow-google-library-reverse-geocoder-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-google-library-reverse-geocoder-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-google-library-reverse-geocoder
        COMMAND shadow-google-library-reverse-geocoder-test
    )

    add_executable(
        shadow-geonames-city-index-test
        tests/geonames_city_index_test.cpp
        src/geonames_city_index.cpp
        src/geonames_city_index.hpp
    )
    target_compile_features(shadow-geonames-city-index-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-geonames-city-index-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-geonames-city-index-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(shadow-geonames-city-index-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-geonames-city-index-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-geonames-city-index
        COMMAND shadow-geonames-city-index-test
    )

    add_executable(
        shadow-geonames-library-reverse-geocoder-test
        tests/geonames_library_reverse_geocoder_test.cpp
        src/geonames_city_index.cpp
        src/geonames_city_index.hpp
        src/geonames_library_reverse_geocoder.cpp
        src/geonames_library_reverse_geocoder.hpp
        src/library_reverse_geocoder.hpp
    )
    target_compile_features(
        shadow-geonames-library-reverse-geocoder-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-geonames-library-reverse-geocoder-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-geonames-library-reverse-geocoder-test
        PRIVATE Qt6::Core Qt6::Concurrent
    )
    if(MSVC)
        target_compile_options(
            shadow-geonames-library-reverse-geocoder-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-geonames-library-reverse-geocoder-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-geonames-library-reverse-geocoder
        COMMAND shadow-geonames-library-reverse-geocoder-test
    )

    add_executable(
        shadow-library-reverse-geocoder-router-test
        tests/library_reverse_geocoder_router_test.cpp
        src/library_reverse_geocoder_router.cpp
        src/library_reverse_geocoder_router.hpp
        src/library_reverse_geocoder.hpp
    )
    target_compile_features(shadow-library-reverse-geocoder-router-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-library-reverse-geocoder-router-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-library-reverse-geocoder-router-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(
            shadow-library-reverse-geocoder-router-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-library-reverse-geocoder-router-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-library-reverse-geocoder-router
        COMMAND shadow-library-reverse-geocoder-router-test
    )

    add_executable(
        shadow-secure-secret-store-test
        tests/secure_secret_store_test.cpp
        src/secure_secret_store.cpp
        src/secure_secret_store.hpp
    )
    target_compile_features(
        shadow-secure-secret-store-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-secure-secret-store-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-secure-secret-store-test
        PRIVATE Qt6::Core
    )
    if(APPLE)
        target_sources(
            shadow-secure-secret-store-test
            PRIVATE src/secure_secret_store_mac.mm
        )
        target_link_libraries(
            shadow-secure-secret-store-test
            PRIVATE "-framework CoreFoundation" "-framework Security"
        )
    else()
        target_sources(
            shadow-secure-secret-store-test
            PRIVATE src/secure_secret_store_stub.cpp
        )
    endif()
    if(MSVC)
        target_compile_options(
            shadow-secure-secret-store-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-secure-secret-store-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-secure-secret-store
        COMMAND shadow-secure-secret-store-test
    )

    add_executable(
        shadow-map-provider-settings-dialog-test
        tests/map_provider_settings_dialog_test.cpp
    )
    target_compile_features(
        shadow-map-provider-settings-dialog-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-map-provider-settings-dialog-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-map-provider-settings-dialog-test
        URI Shadow.MapProviderSettingsContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/MapProviderSettingsDialog.qml
            qml/MapProviderSettingsPane.qml
            qml/ShadowButton.qml
            qml/ShadowSwitch.qml
            qml/Theme.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-map-provider-settings-dialog-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-map-provider-settings-dialog-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-map-provider-settings-dialog
        COMMAND shadow-map-provider-settings-dialog-test
    )
    set_tests_properties(
        shadow-desktop-map-provider-settings-dialog
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-application-settings-dialog-test
        tests/application_settings_dialog_test.cpp
    )
    target_compile_features(
        shadow-application-settings-dialog-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-application-settings-dialog-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-application-settings-dialog-test
        URI Shadow.ApplicationSettingsContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/ApplicationSettingsDialog.qml
            qml/SettingsGeneralPane.qml
            qml/SettingsLibraryPane.qml
            qml/SettingsAiPane.qml
            qml/SettingsStoragePane.qml
            qml/MapProviderSettingsPane.qml
            qml/ShadowButton.qml
            qml/ShadowCheckBox.qml
            qml/ShadowIcon.qml
            qml/ShadowInlineSlider.qml
            qml/ShadowSlider.qml
            qml/ShadowSwitch.qml
            qml/Theme.qml
    )
    qt_add_resources(
        shadow-application-settings-dialog-test
        shadow-application-settings-dialog-test-icons
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES
            icons/add-folder.svg
            icons/check.svg
            icons/map.svg
            icons/mask.svg
            icons/review-grid.svg
            icons/settings.svg
    )
    if(MSVC)
        target_compile_options(
            shadow-application-settings-dialog-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-application-settings-dialog-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-application-settings-dialog
        COMMAND shadow-application-settings-dialog-test
    )
    set_tests_properties(
        shadow-desktop-application-settings-dialog
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-library-map-provider-overlay-test
        tests/library_map_provider_overlay_test.cpp
    )
    target_compile_features(
        shadow-library-map-provider-overlay-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-library-map-provider-overlay-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-library-map-provider-overlay-test
        URI Shadow.LibraryMapProviderOverlayContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/LibraryMapProviderOverlay.qml
            qml/ShadowButton.qml
            qml/Theme.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-library-map-provider-overlay-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-library-map-provider-overlay-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-library-map-provider-overlay
        COMMAND shadow-library-map-provider-overlay-test
    )
    set_tests_properties(
        shadow-desktop-library-map-provider-overlay
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-preview-diagnostics-test
        tests/preview_diagnostics_test.cpp
        src/preview_diagnostics.cpp
        src/preview_diagnostics.hpp
    )
    target_compile_features(shadow-preview-diagnostics-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-preview-diagnostics-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-preview-diagnostics-test PRIVATE Qt6::Gui)
    if(MSVC)
        target_compile_options(shadow-preview-diagnostics-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-preview-diagnostics-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-preview-diagnostics COMMAND shadow-preview-diagnostics-test)

    add_executable(shadow-edit-history-test tests/edit_history_test.cpp)
    target_compile_features(shadow-edit-history-test PRIVATE cxx_std_20)
    target_include_directories(shadow-edit-history-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(shadow-edit-history-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-edit-history-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-edit-history COMMAND shadow-edit-history-test)

    add_executable(
        shadow-edit-history-restore-projection-test
        tests/edit_history_restore_projection_test.cpp
        src/edit_history_restore_projection.cpp
        src/edit_history_restore_projection.hpp
        src/edit_stack.hpp
    )
    target_compile_features(
        shadow-edit-history-restore-projection-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-history-restore-projection-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-history-restore-projection-test
        PRIVATE Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-history-restore-projection-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-history-restore-projection-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-history-restore-projection
        COMMAND shadow-edit-history-restore-projection-test
    )

    add_executable(
        shadow-edit-stack-test
        tests/edit_stack_test.cpp
        src/edit_stack.hpp
    )
    target_compile_features(shadow-edit-stack-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-edit-stack-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-edit-stack-test PRIVATE Qt6::Gui)
    if(MSVC)
        target_compile_options(shadow-edit-stack-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-edit-stack-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-edit-stack COMMAND shadow-edit-stack-test)

    add_executable(
        shadow-lut-preview-provider-test
        tests/lut_preview_provider_test.cpp
        src/lut_preview_provider.cpp
        src/lut_preview_provider.hpp
    )
    target_compile_features(shadow-lut-preview-provider-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-lut-preview-provider-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-lut-preview-provider-test
        PRIVATE Qt6::Quick Shadow::Image
    )
    if(MSVC)
        target_compile_options(
            shadow-lut-preview-provider-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-lut-preview-provider-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-lut-preview-provider
        COMMAND shadow-lut-preview-provider-test
    )

    add_executable(
        shadow-tone-curve-point-model-test
        tests/tone_curve_point_model_test.cpp
        src/tone_curve_point_model.cpp
        src/tone_curve_point_model.hpp
    )
    target_compile_features(shadow-tone-curve-point-model-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-tone-curve-point-model-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-tone-curve-point-model-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(
            shadow-tone-curve-point-model-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-tone-curve-point-model-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-tone-curve-point-model
        COMMAND shadow-tone-curve-point-model-test
    )

    add_executable(
        shadow-edit-version-presentation-test
        tests/edit_version_presentation_test.cpp
        src/edit_version_model.cpp
        src/edit_version_presentation.cpp
    )
    target_compile_features(shadow-edit-version-presentation-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-edit-version-presentation-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-edit-version-presentation-test PRIVATE Qt6::Gui)
    if(MSVC)
        target_compile_options(
            shadow-edit-version-presentation-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-version-presentation-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-version-presentation
        COMMAND shadow-edit-version-presentation-test
    )

    add_executable(
        shadow-edit-preview-contract-test
        tests/edit_preview_contract_test.cpp
        src/edit_mask_coverage_store.cpp
        ${SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE}
        src/edit_preview_presentation_context.cpp
        src/edit_preview_provider.cpp
        src/edit_preview_store.cpp
    )
    target_compile_features(shadow-edit-preview-contract-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-edit-preview-contract-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-edit-preview-contract-test PRIVATE Qt6::Quick)
    if(MSVC)
        target_compile_options(
            shadow-edit-preview-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-preview-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-preview-contract
        COMMAND shadow-edit-preview-contract-test
    )

    add_executable(
        shadow-edit-preview-presentation-context-contract-test
        tests/edit_preview_presentation_context_contract_test.cpp
        src/edit_preview_presentation_context.cpp
    )
    target_compile_features(
        shadow-edit-preview-presentation-context-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-preview-presentation-context-contract-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-preview-presentation-context-contract-test
        PRIVATE Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-preview-presentation-context-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-preview-presentation-context-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-preview-presentation-context-contract
        COMMAND shadow-edit-preview-presentation-context-contract-test
    )

    add_executable(
        shadow-edit-preview-texture-item-contract-test
        tests/edit_preview_texture_item_contract_test.cpp
        ${SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE}
        src/edit_preview_liquify_mesh.cpp
        src/edit_preview_presentation_context.cpp
        src/edit_preview_presentation_registry.cpp
        src/edit_preview_store.cpp
        src/edit_preview_texture_item.cpp
    )
    target_compile_features(
        shadow-edit-preview-texture-item-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-preview-texture-item-contract-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-preview-texture-item-contract-test
        PRIVATE Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-preview-texture-item-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-preview-texture-item-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-preview-texture-item-contract
        COMMAND shadow-edit-preview-texture-item-contract-test
    )

    add_executable(
        shadow-edit-preview-liquify-mesh-test
        tests/edit_preview_liquify_mesh_test.cpp
        src/edit_preview_liquify_mesh.cpp
    )
    target_compile_features(
        shadow-edit-preview-liquify-mesh-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-preview-liquify-mesh-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-preview-liquify-mesh-test
        PRIVATE Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-preview-liquify-mesh-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-preview-liquify-mesh-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-preview-liquify-mesh
        COMMAND shadow-edit-preview-liquify-mesh-test
    )

    add_executable(
        shadow-edit-mask-coverage-store-contract-test
        tests/edit_mask_coverage_store_contract_test.cpp
        src/edit_mask_coverage_store.cpp
        ${SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE}
        src/edit_preview_presentation_context.cpp
        src/edit_preview_provider.cpp
        src/edit_preview_store.cpp
    )
    target_compile_features(
        shadow-edit-mask-coverage-store-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-mask-coverage-store-contract-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-mask-coverage-store-contract-test
        PRIVATE Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-mask-coverage-store-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-mask-coverage-store-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-mask-coverage-store-contract
        COMMAND shadow-edit-mask-coverage-store-contract-test
    )

    add_executable(
        shadow-review-visual-request-contract-test
        tests/review_visual_request_contract_test.cpp
        src/review_visual_request.cpp
    )
    target_compile_features(
        shadow-review-visual-request-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-visual-request-contract-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-visual-request-contract-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-review-visual-request-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-visual-request-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-visual-request-contract
        COMMAND shadow-review-visual-request-contract-test
    )

    add_executable(
        shadow-review-photo-inspection-session-test
        tests/review_photo_inspection_session_test.cpp
        src/review_photo_inspection_session.hpp
    )
    target_compile_features(
        shadow-review-photo-inspection-session-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-photo-inspection-session-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-photo-inspection-session-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-review-photo-inspection-session-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-photo-inspection-session-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-photo-inspection-session
        COMMAND shadow-review-photo-inspection-session-test
    )

    add_executable(
        shadow-review-photo-inspection-coordinator-test
        tests/review_photo_inspection_coordinator_test.cpp
        ${SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES}
        src/review_photo_inspection_coordinator.cpp
        src/review_photo_inspection_coordinator.hpp
        src/review_photo_inspection_session.hpp
        src/desktop_backend.cpp
        src/desktop_backend.hpp
        src/folder_scan_backend.cpp
        src/folder_scan_backend.hpp
        src/photo_inspection_projection.cpp
        src/photo_inspection_projection.hpp
        src/preview_diagnostics.cpp
        src/preview_diagnostics.hpp
    )
    target_compile_features(
        shadow-review-photo-inspection-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-photo-inspection-coordinator-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-review-photo-inspection-coordinator-test
        PRIVATE
            shadow-desktop-export-backend
            Qt6::Concurrent
    )
    add_dependencies(
        shadow-review-photo-inspection-coordinator-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-review-photo-inspection-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-photo-inspection-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-photo-inspection-coordinator
        COMMAND shadow-review-photo-inspection-coordinator-test
    )

    add_executable(
        shadow-backend-edit-settings-projection-test
        tests/backend_edit_settings_projection_test.cpp
        ${SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES}
    )
    target_compile_features(
        shadow-backend-edit-settings-projection-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-backend-edit-settings-projection-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-backend-edit-settings-projection-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-backend-edit-settings-projection-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-backend-edit-settings-projection-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-backend-edit-settings-projection-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-backend-edit-settings-projection
        COMMAND shadow-backend-edit-settings-projection-test
    )

    add_executable(
        shadow-backend-raw-foundation-projection-test
        tests/backend_raw_foundation_projection_test.cpp
        src/backend/raw_foundation_projection.cpp
        src/backend/raw_foundation_projection.hpp
        src/desktop_backend.cpp
        src/desktop_backend.hpp
        src/desktop_backend_library.cpp
        src/desktop_backend_raw_foundation.cpp
        src/folder_scan_backend.cpp
        src/folder_scan_backend.hpp
        src/photo_inspection_projection.cpp
        src/photo_inspection_projection.hpp
        src/preview_diagnostics.cpp
        src/preview_diagnostics.hpp
    )
    target_compile_features(
        shadow-backend-raw-foundation-projection-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-backend-raw-foundation-projection-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-backend-raw-foundation-projection-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-backend-raw-foundation-projection-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-backend-raw-foundation-projection-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-backend-raw-foundation-projection-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-backend-raw-foundation-projection
        COMMAND shadow-backend-raw-foundation-projection-test
    )

    add_executable(
        shadow-edit-liquify-coordinates-test
        tests/edit_liquify_coordinates_test.cpp
        src/edit_liquify_coordinates.cpp
        src/edit_liquify_coordinates.hpp
    )
    target_compile_features(
        shadow-edit-liquify-coordinates-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-liquify-coordinates-test
        PRIVATE src
    )
    target_link_libraries(
        shadow-edit-liquify-coordinates-test
        PRIVATE Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-liquify-coordinates-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-liquify-coordinates-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-liquify-coordinates
        COMMAND shadow-edit-liquify-coordinates-test
    )

    add_executable(
        shadow-review-selection-state-contract-test
        tests/review_selection_state_contract_test.cpp
    )
    target_compile_features(
        shadow-review-selection-state-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-review-selection-state-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-review-selection-state-contract-test
        PRIVATE Qt6::Core Qt6::Qml
    )
    if(MSVC)
        target_compile_options(
            shadow-review-selection-state-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-selection-state-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-selection-state-contract
        COMMAND shadow-review-selection-state-contract-test
    )

    add_executable(
        shadow-review-comparison-state-contract-test
        tests/review_comparison_state_contract_test.cpp
    )
    target_compile_features(
        shadow-review-comparison-state-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-review-comparison-state-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-review-comparison-state-contract-test
        PRIVATE Qt6::Core Qt6::Qml
    )
    if(MSVC)
        target_compile_options(
            shadow-review-comparison-state-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-comparison-state-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-comparison-state-contract
        COMMAND shadow-review-comparison-state-contract-test
    )

    add_executable(
        shadow-autosave-failure-recovery-contract-test
        tests/autosave_failure_recovery_contract_test.cpp
    )
    target_compile_features(
        shadow-autosave-failure-recovery-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-autosave-failure-recovery-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-autosave-failure-recovery-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-autosave-failure-recovery-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-autosave-failure-recovery-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-autosave-failure-recovery-contract
        COMMAND shadow-autosave-failure-recovery-contract-test
    )
    set_tests_properties(
        shadow-desktop-autosave-failure-recovery-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-lut-section-contract-test
        tests/precision_lut_section_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-lut-section-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-precision-lut-section-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-precision-lut-section-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-lut-section-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-lut-section-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-lut-section-contract
        COMMAND shadow-precision-lut-section-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-lut-section-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-mask-create-menu-contract-test
        tests/precision_mask_create_menu_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-mask-create-menu-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-precision-mask-create-menu-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-precision-mask-create-menu-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-mask-create-menu-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-mask-create-menu-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-mask-create-menu-contract
        COMMAND shadow-precision-mask-create-menu-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-mask-create-menu-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-mask-coverage-overlay-contract-test
        tests/precision_mask_coverage_overlay_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-mask-coverage-overlay-contract-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-precision-mask-coverage-overlay-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    qt_add_qml_module(
        shadow-precision-mask-coverage-overlay-contract-test
        URI Shadow.MaskCoverageContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PrecisionMaskCoverageOverlay.qml
            qml/Theme.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-mask-coverage-overlay-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-mask-coverage-overlay-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-mask-coverage-overlay-contract
        COMMAND shadow-precision-mask-coverage-overlay-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-mask-coverage-overlay-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-retouch-tools-contract-test
        tests/precision_retouch_tools_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-retouch-tools-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-precision-retouch-tools-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-precision-retouch-tools-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-retouch-tools-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-retouch-tools-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-retouch-tools-contract
        COMMAND shadow-precision-retouch-tools-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-retouch-tools-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-edit-ai-mask-prompt-state-test
        tests/edit_ai_mask_prompt_state_test.cpp
        src/edit_ai_mask_prompt_state.cpp
        src/edit_ai_mask_prompt_state.hpp
    )
    target_compile_features(
        shadow-edit-ai-mask-prompt-state-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-ai-mask-prompt-state-test
        PRIVATE src
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-ai-mask-prompt-state-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-ai-mask-prompt-state-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-ai-mask-prompt-state
        COMMAND shadow-edit-ai-mask-prompt-state-test
    )

    add_executable(
        shadow-edit-raw-foundation-state-test
        tests/edit_raw_foundation_state_test.cpp
        src/edit_raw_foundation_state.cpp
        src/edit_raw_foundation_state.hpp
    )
    target_compile_features(
        shadow-edit-raw-foundation-state-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-raw-foundation-state-test
        PRIVATE src
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-raw-foundation-state-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-raw-foundation-state-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-raw-foundation-state
        COMMAND shadow-edit-raw-foundation-state-test
    )

    add_executable(
        shadow-precision-ai-mask-prompt-overlay-contract-test
        tests/precision_ai_mask_prompt_overlay_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-ai-mask-prompt-overlay-contract-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-precision-ai-mask-prompt-overlay-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    qt_add_qml_module(
        shadow-precision-ai-mask-prompt-overlay-contract-test
        URI Shadow.AiMaskPromptOverlayContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PrecisionAiMaskPromptOverlay.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-ai-mask-prompt-overlay-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-ai-mask-prompt-overlay-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-ai-mask-prompt-overlay-contract
        COMMAND shadow-precision-ai-mask-prompt-overlay-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-ai-mask-prompt-overlay-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-detail-loupe-contract-test
        tests/precision_detail_loupe_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-detail-loupe-contract-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-precision-detail-loupe-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-precision-detail-loupe-contract-test
        URI Shadow.DetailLoupeContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PrecisionDetailLoupe.qml
            qml/PrecisionDetailLoupeState.qml
            qml/ShadowButton.qml
            qml/ShadowIcon.qml
            qml/ShadowIconButton.qml
            qml/Theme.qml
    )
    qt_add_resources(
        shadow-precision-detail-loupe-contract-test
        shadow-precision-detail-loupe-contract-test-icons
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES
            icons/close.svg
            icons/pin.svg
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-detail-loupe-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-detail-loupe-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-detail-loupe-contract
        COMMAND shadow-precision-detail-loupe-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-detail-loupe-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-raw-foundation-adjustments-contract-test
        tests/precision_raw_foundation_adjustments_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-raw-foundation-adjustments-contract-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-precision-raw-foundation-adjustments-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-precision-raw-foundation-adjustments-contract-test
        URI Shadow.RawFoundationAdjustmentsContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PrecisionFoundationAdjustments.qml
            qml/PrecisionRawDenoiseAdjustments.qml
            qml/ShadowAdjustmentSection.qml
            qml/ShadowButton.qml
            qml/ShadowCheckBox.qml
            qml/ShadowIcon.qml
            qml/ShadowIconButton.qml
            qml/ShadowInlineSlider.qml
            qml/ShadowSlider.qml
            qml/Theme.qml
            qml/ToneCurveEditor.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-raw-foundation-adjustments-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-raw-foundation-adjustments-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-raw-foundation-adjustments-contract
        COMMAND shadow-precision-raw-foundation-adjustments-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-raw-foundation-adjustments-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-adjustment-controls-contract-test
        tests/shadow_adjustment_controls_contract_test.cpp
    )
    target_compile_features(
        shadow-adjustment-controls-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-adjustment-controls-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-adjustment-controls-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    if(MSVC)
        target_compile_options(
            shadow-adjustment-controls-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-adjustment-controls-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-adjustment-controls-contract
        COMMAND shadow-adjustment-controls-contract-test
    )
    set_tests_properties(
        shadow-desktop-adjustment-controls-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-direct-stroke-interaction-test
        tests/precision_direct_stroke_interaction_test.cpp
        src/edit_stroke_input.cpp
        src/edit_stroke_input.hpp
    )
    target_compile_features(
        shadow-precision-direct-stroke-interaction-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-precision-direct-stroke-interaction-test
        PRIVATE src
    )
    target_link_libraries(
        shadow-precision-direct-stroke-interaction-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    qt_add_qml_module(
        shadow-precision-direct-stroke-interaction-test
        URI Shadow.DirectStrokeContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PrecisionActiveStrokeCoverage.qml
            qml/PrecisionCanvasZoomInput.qml
            qml/PrecisionCanvasPickerInput.qml
            qml/PrecisionCropOverlay.qml
            qml/PrecisionLiquifyOverlay.qml
            qml/PrecisionLocalMaskOverlay.qml
            qml/ShadowIcon.qml
            qml/Theme.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-direct-stroke-interaction-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-direct-stroke-interaction-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-direct-stroke-interaction
        COMMAND shadow-precision-direct-stroke-interaction-test
    )
    set_tests_properties(
        shadow-desktop-precision-direct-stroke-interaction
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-point-color-section-contract-test
        tests/precision_point_color_section_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-point-color-section-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-precision-point-color-section-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-precision-point-color-section-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-point-color-section-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-point-color-section-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-point-color-section-contract
        COMMAND shadow-precision-point-color-section-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-point-color-section-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-review-model-test
        tests/review_model_test.cpp
        tests/review_model/review_model_fixture.hpp
        tests/review_model/role_projection_contract.cpp
        tests/review_model/state_projection_contract.cpp
        tests/review_model/visual_generation_contract.cpp
        tests/review_model/item_projection_contract.cpp
        tests/review_model/snapshot_membership_contract.cpp
        src/review_model.cpp
        src/review_visual_request.cpp
    )
    target_compile_features(shadow-review-model-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-review-model-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-review-model-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(shadow-review-model-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-review-model-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-review-model COMMAND shadow-review-model-test)

    add_executable(
        shadow-review-filter-model-test
        tests/review_filter_model_test.cpp
        src/review_filter_model.cpp
        src/review_filter_model.hpp
        src/review_model.cpp
        src/review_model.hpp
        src/review_visual_request.cpp
        src/review_visual_request.hpp
    )
    target_compile_features(
        shadow-review-filter-model-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-filter-model-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-review-filter-model-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(
            shadow-review-filter-model-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-filter-model-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-filter-model
        COMMAND shadow-review-filter-model-test
    )

    add_executable(
        shadow-justified-review-layout-model-test
        tests/justified_review_layout_model_test.cpp
        src/justified_review_layout_model.cpp
        src/justified_review_layout_model.hpp
        src/review_model.cpp
        src/review_model.hpp
        src/review_visual_request.cpp
        src/review_visual_request.hpp
    )
    target_compile_features(
        shadow-justified-review-layout-model-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-justified-review-layout-model-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-justified-review-layout-model-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-justified-review-layout-model-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-justified-review-layout-model-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-justified-review-layout-model
        COMMAND shadow-justified-review-layout-model-test
    )

    add_executable(
        shadow-review-evidence-session-test
        tests/review_evidence_session_test.cpp
        src/review_evidence_session.hpp
    )
    target_compile_features(shadow-review-evidence-session-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-review-evidence-session-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-review-evidence-session-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(
            shadow-review-evidence-session-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-evidence-session-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-evidence-session
        COMMAND shadow-review-evidence-session-test
    )

    add_executable(
        shadow-review-comparison-coordinator-test
        tests/review_comparison_coordinator_test.cpp
        tests/review_comparison_coordinator/comparison_backend_fixture.hpp
        tests/review_comparison_coordinator/evidence_lifecycle_contract.cpp
        tests/review_comparison_coordinator/failure_lifetime_contract.cpp
        tests/review_comparison_coordinator/presentation_contract.cpp
        tests/review_comparison_coordinator/receipt_validation_contract.cpp
        src/review_comparison_coordinator.cpp
        src/review_comparison_coordinator.hpp
        src/review_evidence_session.hpp
    )
    target_compile_features(
        shadow-review-comparison-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-comparison-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-comparison-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-comparison-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-comparison-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-comparison-coordinator
        COMMAND shadow-review-comparison-coordinator-test
    )

    add_executable(
        shadow-review-decision-coordinator-test
        tests/review_decision_coordinator_test.cpp
        tests/review_decision_coordinator/decision_coordinator_fixture.hpp
        tests/review_decision_coordinator/admission_projection_contract.cpp
        tests/review_decision_coordinator/failure_undo_contract.cpp
        tests/review_decision_coordinator/lifetime_contract.cpp
        src/review_decision_coordinator.cpp
        src/review_decision_coordinator.hpp
        src/review_decision_session.hpp
    )
    target_compile_features(
        shadow-review-decision-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-decision-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-decision-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-decision-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-decision-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-decision-coordinator
        COMMAND shadow-review-decision-coordinator-test
    )

    add_executable(
        shadow-review-source-health-coordinator-test
        tests/review_source_health_coordinator_test.cpp
        tests/review_source_health_coordinator/source_health_fixture.hpp
        tests/review_source_health_coordinator/source_health_refresh_contract.cpp
        tests/review_source_health_coordinator/missing_location_review_contract.cpp
        tests/review_source_health_coordinator/relink_lifetime_contract.cpp
        src/review_source_health_coordinator.cpp
        src/review_source_health_coordinator.hpp
    )
    target_compile_features(
        shadow-review-source-health-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-source-health-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-source-health-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-source-health-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-source-health-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-source-health-coordinator
        COMMAND shadow-review-source-health-coordinator-test
    )

    add_executable(
        shadow-review-library-album-coordinator-test
        tests/review_library_album_coordinator_test.cpp
        tests/review_library_album_coordinator/library_album_fixture.hpp
        tests/review_library_album_coordinator/album_lifecycle_contract.cpp
        tests/review_library_album_coordinator/refresh_failure_lifetime_contract.cpp
        src/review_library_album_coordinator.cpp
        src/review_library_album_coordinator.hpp
    )
    target_compile_features(
        shadow-review-library-album-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-album-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-album-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-album-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-album-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-album-coordinator
        COMMAND shadow-review-library-album-coordinator-test
    )

    add_executable(
        shadow-review-library-keyword-coordinator-test
        tests/review_library_keyword_coordinator_test.cpp
        tests/review_library_keyword_coordinator/library_keyword_fixture.hpp
        tests/review_library_keyword_coordinator/keyword_lifecycle_contract.cpp
        tests/review_library_keyword_coordinator/failure_lifetime_contract.cpp
        src/review_library_keyword_coordinator.cpp
        src/review_library_keyword_coordinator.hpp
    )
    target_compile_features(
        shadow-review-library-keyword-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-keyword-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-keyword-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-keyword-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-keyword-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-keyword-coordinator
        COMMAND shadow-review-library-keyword-coordinator-test
    )

    add_executable(
        shadow-review-library-facet-coordinator-test
        tests/review_library_facet_coordinator_test.cpp
        tests/review_library_facet_coordinator/library_facet_fixture.hpp
        tests/review_library_facet_coordinator/facet_projection_contract.cpp
        tests/review_library_facet_coordinator/coalescing_failure_lifetime_contract.cpp
        src/review_library_facet_coordinator.cpp
        src/review_library_facet_coordinator.hpp
    )
    target_compile_features(
        shadow-review-library-facet-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-facet-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-facet-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-facet-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-facet-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-facet-coordinator
        COMMAND shadow-review-library-facet-coordinator-test
    )

    add_executable(
        shadow-review-library-place-resolution-coordinator-test
        tests/review_library_place_resolution_coordinator_test.cpp
        src/review_library_place_resolution_coordinator.cpp
        src/review_library_place_resolution_coordinator.hpp
        src/library_reverse_geocoder.hpp
    )
    target_compile_features(
        shadow-review-library-place-resolution-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-place-resolution-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-place-resolution-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-place-resolution-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-place-resolution-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-place-resolution-coordinator
        COMMAND shadow-review-library-place-resolution-coordinator-test
    )

    add_executable(
        shadow-review-library-map-coordinator-test
        tests/review_library_map_coordinator_test.cpp
        src/review_library_map_coordinator.cpp
        src/review_library_map_coordinator.hpp
    )
    target_compile_features(
        shadow-review-library-map-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-map-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-map-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-map-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-map-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-map-coordinator
        COMMAND shadow-review-library-map-coordinator-test
    )

    add_executable(
        shadow-library-map-location-placement-test
        tests/library_map_location_placement_test.cpp
    )
    target_compile_features(
        shadow-library-map-location-placement-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-library-map-location-placement-test
        PRIVATE Qt6::Core Qt6::Qml
    )
    qt_add_qml_module(
        shadow-library-map-location-placement-test
        URI Shadow.LibraryMapLocationPlacementContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/LibraryMapLocationPlacementState.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-library-map-location-placement-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-library-map-location-placement-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-library-map-location-placement
        COMMAND shadow-library-map-location-placement-test
    )

    add_executable(
        shadow-review-library-organization-coordinator-test
        tests/review_library_organization_coordinator_test.cpp
        tests/review_library_organization_coordinator/library_organization_fixture.hpp
        tests/review_library_organization_coordinator/mutation_projection_contract.cpp
        tests/review_library_organization_coordinator/failure_lifetime_contract.cpp
        src/review_library_organization_coordinator.cpp
        src/review_library_organization_coordinator.hpp
    )
    target_compile_features(
        shadow-review-library-organization-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-organization-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-organization-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-organization-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-organization-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-organization-coordinator
        COMMAND shadow-review-library-organization-coordinator-test
    )

    add_executable(
        shadow-review-import-coordinator-test
        tests/review_import_coordinator_test.cpp
        tests/review_import_coordinator/import_coordinator_fixture.hpp
        tests/review_import_coordinator/import_progress_contract.cpp
        tests/review_import_coordinator/cancellation_failure_lifetime_contract.cpp
        src/review_import_coordinator.cpp
        src/review_import_coordinator.hpp
    )
    target_compile_features(
        shadow-review-import-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-import-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-import-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-import-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-import-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-import-coordinator
        COMMAND shadow-review-import-coordinator-test
    )

    add_executable(
        shadow-review-library-query-coordinator-test
        tests/review_library_query_coordinator_test.cpp
        tests/review_library_query_coordinator/library_query_fixture.hpp
        tests/review_library_query_coordinator/projection_pagination_contract.cpp
        tests/review_library_query_coordinator/coalescing_failure_lifetime_contract.cpp
        src/review_library_query_coordinator.cpp
        src/review_library_query_coordinator.hpp
        src/review_decision_coordinator.cpp
        src/review_decision_coordinator.hpp
        src/review_model.cpp
        src/review_model.hpp
        src/review_visual_request.cpp
        src/review_visual_request.hpp
    )
    target_compile_features(
        shadow-review-library-query-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-query-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-query-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-query-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-query-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-query-coordinator
        COMMAND shadow-review-library-query-coordinator-test
    )

    add_executable(
        shadow-review-shared-grade-coordinator-test
        tests/review_shared_grade_coordinator_test.cpp
        tests/review_shared_grade_coordinator/shared_grade_fixture.hpp
        tests/review_shared_grade_coordinator/projection_apply_contract.cpp
        tests/review_shared_grade_coordinator/failure_admission_contract.cpp
        src/review_shared_grade_coordinator.cpp
        src/review_shared_grade_coordinator.hpp
    )
    target_compile_features(
        shadow-review-shared-grade-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-shared-grade-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-shared-grade-coordinator-test
        PRIVATE Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-shared-grade-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-shared-grade-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-shared-grade-coordinator
        COMMAND shadow-review-shared-grade-coordinator-test
    )

    add_executable(
        shadow-review-decision-session-test
        tests/review_decision_session_test.cpp
        src/review_decision_session.hpp
    )
    target_compile_features(shadow-review-decision-session-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-review-decision-session-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-review-decision-session-test PRIVATE Qt6::Gui)
    if(MSVC)
        target_compile_options(
            shadow-review-decision-session-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-decision-session-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-decision-session
        COMMAND shadow-review-decision-session-test
    )
endif()

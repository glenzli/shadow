find_package(
    Qt6 6.11.1 REQUIRED
    COMPONENTS Core Concurrent LinguistTools Quick QuickControls2 Svg Widgets
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
    "${PROJECT_SOURCE_DIR}/cpp/shadow-image/src/bridge/cxx_bridge.cpp"
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

qt_add_executable(
    shadow-desktop
    MACOSX_BUNDLE
    ${SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES}
    src/desktop_backend.cpp
    src/desktop_backend_cache.cpp
    src/desktop_backend_edit.cpp
    src/desktop_backend_library.cpp
    src/desktop_backend_review.cpp
    src/desktop_backend.hpp
    src/backend/desktop_backend_private.hpp
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
    src/edit_analysis_controller.cpp
    src/edit_detail_render_controller.cpp
    src/edit_controller.hpp
    src/edit_fine_parameter_registry.cpp
    src/edit_fine_parameter_registry.hpp
    src/edit_point_color_controller.cpp
    src/edit_point_color_model.hpp
    src/edit_geometry_controller.cpp
    src/edit_grade_node_controller.cpp
    src/edit_history_controller.cpp
    src/edit_history_restore_projection.cpp
    src/edit_local_mask_controller.cpp
    src/edit_mask_assignment_controller.cpp
    src/edit_optics_controller.cpp
    src/edit_persistence_coordinator.cpp
    src/edit_render_coordinator.cpp
    src/edit_retouch_controller.cpp
    src/edit_task_runner.cpp
    src/edit_task_runner.hpp
    src/edit_tone_curve_controller.cpp
    src/edit_history.hpp
    src/edit_preview_contract.hpp
    src/edit_preview_provider.cpp
    src/edit_preview_provider.hpp
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
    src/justified_review_layout_model.cpp
    src/justified_review_layout_model.hpp
    src/main.cpp
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
        icons/mask.svg
        icons/mask-add.svg
        icons/mask-color-range.svg
        icons/mask-intersect.svg
        icons/mask-linear.svg
        icons/mask-luminance-range.svg
        icons/mask-radial.svg
        icons/mask-subtract.svg
        icons/metadata.svg
        icons/move-down.svg
        icons/move-up.svg
        icons/node-add.svg
        icons/overlay-show.svg
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
    )
    target_link_libraries(shadow-desktop PRIVATE "-framework AppKit")
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
        qml/LibraryAlbumCreateDialog.qml
        qml/LibraryAlbumDeleteDialog.qml
        qml/LibraryAlbumDialogs.qml
        qml/LibraryAlbumManageDialog.qml
        qml/LibraryAlbumMembershipDialog.qml
        qml/LibraryAdvancedFilterPopup.qml
        qml/LibraryFacetPopup.qml
        qml/LibraryImportPane.qml
        qml/LibrarySourceHealthPane.qml
        qml/LibraryWorkspace.qml
        qml/LutManagerWindow.qml
        qml/Main.qml
        qml/MainTitleBar.qml
        qml/MainLibraryFilterBar.qml
        qml/MainPrecisionProxyStatus.qml
        qml/MainSelectionDecisionBar.qml
        qml/MainStatusBar.qml
        qml/AutosaveFailureRecovery.qml
        qml/MetadataWindow.qml
        qml/OpticsProfileManagerWindow.qml
        qml/PreferencesMenu.qml
        qml/PrecisionCanvas.qml
        qml/PrecisionCanvasPickerInput.qml
        qml/PrecisionCanvasStatusOverlays.qml
        qml/PrecisionCanvasToolbar.qml
        qml/PrecisionCanvasZoomInput.qml
        qml/PrecisionCaptureMetadata.qml
        qml/PrecisionComparisonSurface.qml
        qml/PrecisionInspector.qml
        qml/PrecisionResetAllDialog.qml
        qml/PrecisionLutSection.qml
        qml/PrecisionPointColorSection.qml
        qml/PrecisionFoundationAdjustments.qml
        qml/PrecisionColorMixer.qml
        qml/PrecisionSelectiveColor.qml
        qml/ColorWarperEditor.qml
        qml/CacheMaintenanceWindow.qml
        qml/HueCurveEditor.qml
        qml/PrecisionGeometryTools.qml
        qml/PrecisionGradeNodeMenus.qml
        qml/PrecisionGradeNodePane.qml
        qml/PrecisionLocalMaskOverlay.qml
        qml/PrecisionCropOverlay.qml
        qml/PrecisionRetouchOverlay.qml
        qml/PrecisionRetouchSpotHandle.qml
        qml/PrecisionRetouchStrokeCoverage.qml
        qml/PrecisionRetouchStrokeHandle.qml
        qml/PrecisionLocalMaskTools.qml
        qml/PrecisionMaskCreateMenu.qml
        qml/PrecisionRetouchTools.qml
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
        qml/ReviewSystemCollections.qml
        qml/ReviewSharedGradePicker.qml
        qml/ReviewPhotoCard.qml
        qml/ReviewComparisonSlots.qml
        qml/ReviewPhotoContextMenu.qml
        qml/ReviewExifSection.qml
        qml/ReviewPhotoInspector.qml
        qml/ReviewPhotoSummary.qml
        qml/ReviewSinglePreview.qml
        qml/ShadowButton.qml
        qml/ShadowColorLabelButton.qml
        qml/ShadowColorWheel.qml
        qml/ShadowAdjustmentSection.qml
        qml/ShadowHueRange.qml
        qml/ShadowIcon.qml
        qml/ShadowIconButton.qml
        qml/ShadowInlineSlider.qml
        qml/ShadowSlider.qml
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
        src/edit_preview_provider.cpp
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

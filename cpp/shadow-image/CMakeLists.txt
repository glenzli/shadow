find_package(PkgConfig REQUIRED)
#Parallel source sessions are safe only with LibRaw's reentrant Unix/macOS build.
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
find_package(JPEG REQUIRED)

add_library(shadow-image-platform-runtime INTERFACE)
add_library(Shadow::ImagePlatformRuntime ALIAS shadow-image-platform-runtime)

function(shadow_image_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wsign-conversion
        )
    endif()
endfunction()

set(SHADOW_IMAGE_SOURCE_MANIFEST_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/cmake/source-manifests")

function(shadow_image_read_source_manifest output_variable manifest_name)
    set(manifest_path "${SHADOW_IMAGE_SOURCE_MANIFEST_DIRECTORY}/${manifest_name}")
    if(NOT EXISTS "${manifest_path}")
        message(FATAL_ERROR "Missing shadow-image source manifest: ${manifest_path}")
    endif()
    set_property(
        DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest_path}"
    )
    file(STRINGS "${manifest_path}" manifest_entries)
    set(sources)
    foreach(entry IN LISTS manifest_entries)
        string(STRIP "${entry}" entry)
        if(entry STREQUAL "" OR entry MATCHES "^#")
            continue()
        endif()
        if(
            NOT entry MATCHES "^src/.+\\.(cpp|mm)$"
            OR entry MATCHES "(^|/)\\.\\.?(/|$)"
        )
            message(FATAL_ERROR "Invalid source path '${entry}' in ${manifest_path}")
        endif()
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${entry}")
            message(FATAL_ERROR "Source '${entry}' from ${manifest_path} does not exist")
        endif()
        list(APPEND sources "${entry}")
    endforeach()
    if(NOT sources)
        message(FATAL_ERROR "Source manifest ${manifest_path} is empty")
    endif()
    set(${output_variable} "${sources}" PARENT_SCOPE)
endfunction()

shadow_image_read_source_manifest(SHADOW_IMAGE_PORTABLE_SOURCES portable.txt)
shadow_image_read_source_manifest(SHADOW_IMAGE_APPLE_SOURCES apple.txt)
shadow_image_read_source_manifest(SHADOW_IMAGE_NON_APPLE_SOURCES non-apple.txt)
shadow_image_read_source_manifest(SHADOW_IMAGE_METAL_SOURCES metal.txt)
shadow_image_read_source_manifest(SHADOW_IMAGE_METAL_STUB_SOURCES metal-stubs.txt)

set(
    SHADOW_IMAGE_ALL_MANIFEST_SOURCES
    ${SHADOW_IMAGE_PORTABLE_SOURCES}
    ${SHADOW_IMAGE_APPLE_SOURCES}
    ${SHADOW_IMAGE_NON_APPLE_SOURCES}
    ${SHADOW_IMAGE_METAL_SOURCES}
    ${SHADOW_IMAGE_METAL_STUB_SOURCES}
)
list(LENGTH SHADOW_IMAGE_ALL_MANIFEST_SOURCES SHADOW_IMAGE_SOURCE_COUNT)
list(REMOVE_DUPLICATES SHADOW_IMAGE_ALL_MANIFEST_SOURCES)
list(LENGTH SHADOW_IMAGE_ALL_MANIFEST_SOURCES SHADOW_IMAGE_UNIQUE_SOURCE_COUNT)
if(NOT SHADOW_IMAGE_SOURCE_COUNT EQUAL SHADOW_IMAGE_UNIQUE_SOURCE_COUNT)
    message(FATAL_ERROR
        "A shadow-image source occurs in more than one source manifest"
    )
endif()

add_library(shadow-image STATIC ${SHADOW_IMAGE_PORTABLE_SOURCES})
if(APPLE)
    find_library(SHADOW_COREML_FRAMEWORK CoreML REQUIRED)
    find_library(SHADOW_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    target_sources(
        shadow-image
        PRIVATE
            ${SHADOW_IMAGE_APPLE_SOURCES}
    )
    target_compile_definitions(shadow-image PRIVATE SHADOW_IMAGE_HAS_COREML=1)
    target_link_libraries(
        shadow-image-platform-runtime
        INTERFACE
            "${SHADOW_COREML_FRAMEWORK}"
            "${SHADOW_FOUNDATION_FRAMEWORK}"
    )
else()
    target_sources(
        shadow-image
        PRIVATE
            ${SHADOW_IMAGE_NON_APPLE_SOURCES}
    )
    target_compile_definitions(shadow-image PRIVATE SHADOW_IMAGE_HAS_COREML=0)
endif()
if(APPLE AND SHADOW_ENABLE_METAL)
    find_library(SHADOW_METAL_FRAMEWORK Metal REQUIRED)
    target_sources(
        shadow-image
        PRIVATE
            ${SHADOW_IMAGE_METAL_SOURCES}
    )
    target_compile_definitions(shadow-image PRIVATE SHADOW_IMAGE_HAS_METAL=1)
    target_link_libraries(
        shadow-image-platform-runtime
        INTERFACE
            "${SHADOW_METAL_FRAMEWORK}"
    )
else()
    target_sources(
        shadow-image
        PRIVATE
            ${SHADOW_IMAGE_METAL_STUB_SOURCES}
    )
    target_compile_definitions(shadow-image PRIVATE SHADOW_IMAGE_HAS_METAL=0)
endif()
add_library(Shadow::Image ALIAS shadow-image)
target_compile_features(shadow-image PUBLIC cxx_std_20)
set_target_properties(shadow-image PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(
    shadow-image
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    shadow-image
    PRIVATE PkgConfig::LibRaw PkgConfig::LittleCms JPEG::JPEG ${CMAKE_DL_LIBS}
    PUBLIC Shadow::ImagePlatformRuntime
)
if(Lensfun_FOUND)
#An explicitly selected Lensfun prefix must win over broad dependency include roots such as
#/ opt / homebrew / include.Otherwise a developer database build can compile against Homebrew's
#headers and link a different ABI - compatible - looking library from the selected prefix.
    target_include_directories(
        shadow-image
        SYSTEM BEFORE PRIVATE
            ${Lensfun_INCLUDE_DIRS}
    )
    target_link_libraries(shadow-image PRIVATE PkgConfig::Lensfun)
    target_compile_definitions(shadow-image PRIVATE SHADOW_IMAGE_HAS_LENSFUN=1)
else()
    target_compile_definitions(shadow-image PRIVATE SHADOW_IMAGE_HAS_LENSFUN=0)
endif()
if(LibHeif_FOUND)
    target_link_libraries(shadow-image PRIVATE PkgConfig::LibHeif)
    target_compile_definitions(shadow-image PRIVATE SHADOW_IMAGE_HAS_LIBHEIF=1)
else()
    target_compile_definitions(shadow-image PRIVATE SHADOW_IMAGE_HAS_LIBHEIF=0)
endif()
shadow_image_enable_warnings(shadow-image)

if(SHADOW_BUILD_RAW_PROBE)
    add_executable(shadow-raw-probe tools/raw_probe.cpp)
    target_link_libraries(shadow-raw-probe PRIVATE Shadow::Image)
    target_include_directories(
        shadow-raw-probe
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/raw/neural_raw_denoise"
    )
    shadow_image_enable_warnings(shadow-raw-probe)
endif()

#The desktop starts this tiny executable for untrusted or proprietary RAW
#decoder work.A native crash in LibRaw or a locally installed vendor module
#then becomes one failed preview instead of a crashed photo application.
add_executable(shadow-image-decode-helper tools/decode_helper.cpp)
target_link_libraries(shadow-image-decode-helper PRIVATE Shadow::Image)
shadow_image_enable_warnings(shadow-image-decode-helper)

if(BUILD_TESTING)
#This deliberately contains no vendor SDK.It exercises the same load / create / destroy
#contract a user's private SDK adapter will use, without allowing public tests to depend
#on proprietary headers or a real camera file.
    add_library(
        shadow-image-private-decoder-test-plugin
        MODULE
        tests/private_decoder_test_plugin.cpp
    )
    target_compile_features(shadow-image-private-decoder-test-plugin PRIVATE cxx_std_20)
    target_include_directories(
        shadow-image-private-decoder-test-plugin
        PRIVATE
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    )
    shadow_image_enable_warnings(shadow-image-private-decoder-test-plugin)

    add_test(
        NAME shadow-image-neutral-detail-helper-contract
        COMMAND
            ${CMAKE_COMMAND}
            "-DDECODE_HELPER=$<TARGET_FILE:shadow-image-decode-helper>"
            "-DPRIVATE_PROVIDER=$<TARGET_FILE:shadow-image-private-decoder-test-plugin>"
            "-DTEST_OUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/contract-output"
            -P
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/neutral_detail_helper_contract.cmake"
    )
    set_tests_properties(
        shadow-image-neutral-detail-helper-contract
        PROPERTIES
            LABELS "decoder-helper;private-provider;detail"
            TIMEOUT 30
    )

#Models an old v1 - only artifact that predates the exact interface seal.Its factory aborts;
#the loader contract test must reject it before construction.
    add_library(
        shadow-image-stale-private-decoder-test-plugin
        MODULE
        tests/stale_private_decoder_test_plugin.cpp
    )
    target_compile_features(
        shadow-image-stale-private-decoder-test-plugin
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-image-stale-private-decoder-test-plugin
        PRIVATE
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    )
    shadow_image_enable_warnings(shadow-image-stale-private-decoder-test-plugin)

#A public, LibRaw - backed dynamic provider used only to validate the local - provider route.
#It deliberately resembles the shape of a private vendor plugin without containing one.
    add_library(
        shadow-image-libraw-dummy-private-provider
        MODULE
        tests/libraw_dummy_private_decoder_provider.cpp
    )
    target_compile_features(shadow-image-libraw-dummy-private-provider PRIVATE cxx_std_20)
    target_link_libraries(shadow-image-libraw-dummy-private-provider PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-libraw-dummy-private-provider)

    function(shadow_image_add_contract target_name test_name source_name)
        add_executable(${target_name} ${source_name})
        target_link_libraries(${target_name} PRIVATE Shadow::Image)
        shadow_image_enable_warnings(${target_name})
        add_test(NAME ${test_name} COMMAND ${target_name})
    endfunction()

    shadow_image_add_contract(
        shadow-image-raw-frame-tests
        shadow-image-raw-frame-contract
        tests/raw_frame_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-sensor-clipping-tests
        shadow-image-sensor-clipping-contract
        tests/sensor_clipping_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-bayer-demosaic-tests
        shadow-image-bayer-demosaic-contract
        tests/bayer_demosaic_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-raw-development-plan-tests
        shadow-image-raw-development-plan-contract
        tests/raw_development_plan_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-decoder-types-tests
        shadow-image-decoder-types-contract
        tests/decoder_types_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-decoder-error-tests
        shadow-image-decoder-error-contract
        tests/decoder_error_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-decoder-metadata-tests
        shadow-image-decoder-metadata-contract
        tests/decoder_metadata_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-focus-observation-tests
        shadow-image-focus-observation-contract
        tests/focus_observation_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-auto-geometry-tests
        shadow-image-auto-geometry-contract
        tests/auto_geometry_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-raster-gps-metadata-tests
        shadow-image-raster-gps-metadata-contract
        tests/raster_gps_metadata_contract_test.cpp
    )
    target_include_directories(
        shadow-image-raster-gps-metadata-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder"
    )
    shadow_image_add_contract(
        shadow-image-libraw-reference-development-tests
        shadow-image-libraw-reference-development-contract
        tests/libraw_reference_development_contract_test.cpp
    )
    target_include_directories(
        shadow-image-libraw-reference-development-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder"
    )
    shadow_image_add_contract(
        shadow-image-color-management-tests
        shadow-image-color-management-contract
        tests/color_management_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-raster-provider-tests
        shadow-image-raster-provider-contract
        tests/raster_provider_contract_test.cpp
    )
    target_compile_definitions(
        shadow-image-raster-provider-tests
        PRIVATE
            "SHADOW_TEST_JPEG_PATH=\"${CMAKE_SOURCE_DIR}/apps/desktop/assets/lut-preview-reference.jpg\""
    )

    shadow_image_add_contract(
        shadow-image-private-decoder-tests
        shadow-image-private-decoder-contract
        tests/private_decoder_contract_test.cpp
    )
    add_dependencies(
        shadow-image-private-decoder-tests
        shadow-image-private-decoder-test-plugin
        shadow-image-stale-private-decoder-test-plugin
        shadow-image-libraw-dummy-private-provider
    )
    target_compile_definitions(
        shadow-image-private-decoder-tests
        PRIVATE
            "SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH=\"$<TARGET_FILE:shadow-image-private-decoder-test-plugin>\""
            "SHADOW_TEST_STALE_PRIVATE_DECODER_PLUGIN_PATH=\"$<TARGET_FILE:shadow-image-stale-private-decoder-test-plugin>\""
            "SHADOW_TEST_LIBRAW_DUMMY_PRIVATE_DECODER_PLUGIN_PATH=\"$<TARGET_FILE:shadow-image-libraw-dummy-private-provider>\""
    )

    shadow_image_add_contract(
        shadow-image-optics-preparation-tests
        shadow-image-optics-preparation-contract
        tests/optics_preparation_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-optics-region-tests
        shadow-image-optics-region-contract
        tests/optics_region_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-lensfun-region-plan-tests
        shadow-image-lensfun-region-plan-contract
        tests/lensfun_region_plan_contract_test.cpp
    )
    if(APPLE AND SHADOW_ENABLE_METAL)
        shadow_image_add_contract(
            shadow-image-manual-optics-metal-execution-tests
            shadow-image-manual-optics-metal-execution-contract
            tests/manual_optics_metal_execution_contract_test.cpp
        )
        target_include_directories(
            shadow-image-manual-optics-metal-execution-tests
            PRIVATE
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        )
        set_tests_properties(
            shadow-image-manual-optics-metal-execution-contract
            PROPERTIES
                ENVIRONMENT "SHADOW_TEST_REQUIRE_METAL=1"
                LABELS "metal-required"
        )
    endif()
    shadow_image_add_contract(
        shadow-image-lensfun-profile-catalog-tests
        shadow-image-lensfun-profile-catalog-contract
        tests/lensfun_profile_catalog_contract_test.cpp
    )
    if(Lensfun_FOUND)
        target_include_directories(
            shadow-image-lensfun-profile-catalog-tests
            SYSTEM BEFORE PRIVATE
                ${Lensfun_INCLUDE_DIRS}
        )
        target_compile_definitions(
            shadow-image-lensfun-profile-catalog-tests
            PRIVATE SHADOW_IMAGE_HAS_LENSFUN=1
        )
    else()
        target_compile_definitions(
            shadow-image-lensfun-profile-catalog-tests
            PRIVATE SHADOW_IMAGE_HAS_LENSFUN=0
        )
    endif()
    if(Lensfun_FOUND)
        file(
            GLOB SHADOW_LENSFUN_DATABASE_DIRECTORIES
            LIST_DIRECTORIES TRUE
            "${Lensfun_PREFIX}/share/lensfun/version_*"
        )
        if(SHADOW_LENSFUN_DATABASE_DIRECTORIES)
            list(SORT SHADOW_LENSFUN_DATABASE_DIRECTORIES COMPARE NATURAL ORDER DESCENDING)
            list(GET SHADOW_LENSFUN_DATABASE_DIRECTORIES 0 SHADOW_LENSFUN_TEST_DATABASE)
            set_tests_properties(
                shadow-image-optics-preparation-contract
                shadow-image-lensfun-profile-catalog-contract
                shadow-image-lensfun-region-plan-contract
                PROPERTIES ENVIRONMENT
                    "SHADOW_TEST_LENSFUN_DB=$<SHELL_PATH:${SHADOW_LENSFUN_TEST_DATABASE}>"
            )
        endif()
    endif()

    shadow_image_add_contract(
        shadow-image-proxy-output-tests
        shadow-image-proxy-output-contract
        tests/proxy_output_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-developed-source-raster-tests
        shadow-image-developed-source-raster-contract
        tests/developed_source_raster_contract_test.cpp
    )
    target_include_directories(
        shadow-image-developed-source-raster-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy"
    )
    shadow_image_add_contract(
        shadow-image-proxy-render-request-validation-tests
        shadow-image-proxy-render-request-validation-contract
        tests/proxy_render_request_validation_contract_test.cpp
    )
    target_include_directories(
        shadow-image-proxy-render-request-validation-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy"
    )
    shadow_image_add_contract(
        shadow-image-edit-preview-session-tests
        shadow-image-edit-preview-session-contract
        tests/edit_preview_session_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-edit-preview-frame-tests
        shadow-image-edit-preview-frame-contract
        tests/edit_preview_frame_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-edit-preview-execution-tests
        shadow-image-edit-preview-execution-contract
        tests/edit_preview_execution_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-edit-preview-layer-execution-tests
        shadow-image-edit-preview-layer-execution-contract
        tests/edit_preview_layer_execution_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-local-mask-coverage-tests
        shadow-image-local-mask-coverage-contract
        tests/local_mask_coverage_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-managed-raster-mask-tests
        shadow-image-managed-raster-mask-contract
        tests/managed_raster_mask_contract_test.cpp
    )
    target_include_directories(
        shadow-image-managed-raster-mask-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/edit"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy"
    )
    shadow_image_add_contract(
        shadow-image-edit-preview-geometry-execution-tests
        shadow-image-edit-preview-geometry-execution-contract
        tests/edit_preview_geometry_execution_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-edit-preview-liquify-execution-tests
        shadow-image-edit-preview-liquify-execution-contract
        tests/edit_preview_liquify_execution_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-warm-edit-gpu-render-plan-tests
        shadow-image-warm-edit-gpu-render-plan-contract
        tests/warm_edit_gpu_render_plan_contract_test.cpp
    )
    target_include_directories(
        shadow-image-warm-edit-gpu-render-plan-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy"
    )
    shadow_image_add_contract(
        shadow-image-warm-edit-gpu-brush-index-tests
        shadow-image-warm-edit-gpu-brush-index-contract
        tests/warm_edit_gpu_brush_index_contract_test.cpp
    )
    target_include_directories(
        shadow-image-warm-edit-gpu-brush-index-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy"
    )
    shadow_image_add_contract(
        shadow-image-warm-edit-gpu-retouch-plan-tests
        shadow-image-warm-edit-gpu-retouch-plan-contract
        tests/warm_edit_gpu_retouch_plan_contract_test.cpp
    )
    target_include_directories(
        shadow-image-warm-edit-gpu-retouch-plan-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy"
    )
    shadow_image_add_contract(
        shadow-image-warm-edit-gpu-geometry-plan-tests
        shadow-image-warm-edit-gpu-geometry-plan-contract
        tests/warm_edit_gpu_geometry_plan_contract_test.cpp
    )
    target_include_directories(
        shadow-image-warm-edit-gpu-geometry-plan-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy"
    )
    shadow_image_add_contract(
        shadow-image-libraw-provider-tests
        shadow-image-libraw-provider-contract
        tests/libraw_provider_contract_test.cpp
    )

    shadow_image_add_contract(
        shadow-image-edit-execution-plan-tests
        shadow-image-edit-execution-plan-contract
        tests/edit_execution_plan_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-core-adjustment-execution-tests
        shadow-image-core-adjustment-execution-contract
        tests/core_adjustment_execution_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-edit-input-validation-tests
        shadow-image-edit-input-validation-contract
        tests/edit_input_validation_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-guided-selective-tone-tests
        shadow-image-guided-selective-tone-contract
        tests/guided_selective_tone_contract_test.cpp
    )
    target_include_directories(
        shadow-image-guided-selective-tone-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/edit"
    )
    shadow_image_add_contract(
        shadow-image-perceptual-contrast-tests
        shadow-image-perceptual-contrast-contract
        tests/perceptual_contrast_contract_test.cpp
    )
    target_include_directories(
        shadow-image-perceptual-contrast-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/edit"
    )
    shadow_image_add_contract(
        shadow-image-tone-curve-tests
        shadow-image-tone-curve-contract
        tests/tone_curve_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-scalar-neighborhood-filters-tests
        shadow-image-scalar-neighborhood-filters-contract
        tests/scalar_neighborhood_filters_contract_test.cpp
    )
    target_include_directories(
        shadow-image-scalar-neighborhood-filters-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/edit"
    )
    shadow_image_add_contract(
        shadow-image-finishing-effects-tests
        shadow-image-finishing-effects-contract
        tests/finishing_effects_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-technical-detail-tests
        shadow-image-technical-detail-contract
        tests/technical_detail_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-creative-detail-grading-tests
        shadow-image-creative-detail-grading-contract
        tests/creative_detail_grading_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-detail-effects-tests
        shadow-image-detail-effects-contract
        tests/detail_effects_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-lut-execution-tests
        shadow-image-lut-execution-contract
        tests/lut_execution_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-photo-liquify-tests
        shadow-image-photo-liquify-contract
        tests/photo_liquify_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-photo-structural-rendering-tests
        shadow-image-photo-structural-rendering-contract
        tests/photo_structural_rendering_contract_test.cpp
    )

    add_executable(
        shadow-image-spatial-edit-tests
        tests/spatial_edit_contract_test.cpp
    )
    target_link_libraries(
        shadow-image-spatial-edit-tests
        PRIVATE Shadow::Image
    )
    shadow_image_enable_warnings(shadow-image-spatial-edit-tests)
    add_test(
        NAME shadow-image-spatial-edit-contract
        COMMAND shadow-image-spatial-edit-tests
    )
    shadow_image_add_contract(
        shadow-image-retouch-quality-tests
        shadow-image-retouch-quality-contract
        tests/retouch_quality_contract_test.cpp
    )

    add_executable(
        shadow-image-perceptual-color-tests
        tests/perceptual_color_contract_test.cpp
    )
    target_link_libraries(
        shadow-image-perceptual-color-tests
        PRIVATE Shadow::Image
    )
    shadow_image_enable_warnings(shadow-image-perceptual-color-tests)
    add_test(
        NAME shadow-image-perceptual-color-contract
        COMMAND shadow-image-perceptual-color-tests
    )
    target_include_directories(
        shadow-image-perceptual-color-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/edit"
    )
    shadow_image_add_contract(
        shadow-image-point-color-tests
        shadow-image-point-color-contract
        tests/point_color_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-selective-color-tests
        shadow-image-selective-color-contract
        tests/selective_color_contract_test.cpp
    )

    shadow_image_add_contract(
        shadow-image-oklab-color-warper-tests
        shadow-image-oklab-color-warper-contract
        tests/oklab_color_warper_contract_test.cpp
    )
    target_include_directories(
        shadow-image-oklab-color-warper-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/edit"
    )
    shadow_image_add_contract(
        shadow-image-metal-adjustment-program-tests
        shadow-image-metal-adjustment-program-contract
        tests/metal_adjustment_program_contract_test.cpp
    )
    target_include_directories(
        shadow-image-metal-adjustment-program-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/edit"
    )

    add_executable(
        shadow-image-neutral-balance-tests
        tests/neutral_balance_contract_test.cpp
    )
    target_link_libraries(shadow-image-neutral-balance-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-neutral-balance-tests)
    add_test(
        NAME shadow-image-neutral-balance-contract
        COMMAND shadow-image-neutral-balance-tests
    )

    add_executable(
        shadow-image-adjustment-execution-tests
        tests/adjustment_execution_contract_test.cpp
        tests/adjustment_execution_contract/execution_parity_fixture.cpp
        tests/adjustment_execution_contract/advanced_operation_fixture.cpp
        tests/adjustment_execution_contract/perceptual_operation_fixture.cpp
        tests/adjustment_execution_contract/dispatch_fallback_contract_test.cpp
        tests/adjustment_execution_contract/operation_locality_contract_test.cpp
        tests/adjustment_execution_contract/backend_resource_contract_test.cpp
        tests/adjustment_execution_contract/advanced_perceptual_parity_contract_test.cpp
        tests/adjustment_execution_contract/core_operation_parity_contract_test.cpp
    )
    target_link_libraries(
        shadow-image-adjustment-execution-tests
        PRIVATE Shadow::Image
    )
    shadow_image_enable_warnings(shadow-image-adjustment-execution-tests)
    add_test(
        NAME shadow-image-adjustment-execution-contract
        COMMAND shadow-image-adjustment-execution-tests
    )

    add_executable(
        shadow-image-warm-edit-gpu-tests
        tests/warm_edit_gpu_contract_test.cpp
        tests/warm_edit_gpu_contract/warm_edit_gpu_parity_fixture.cpp
        tests/warm_edit_gpu_contract/resident_session_contract_test.cpp
        tests/warm_edit_gpu_contract/technical_detail_stage_contract_test.cpp
        tests/warm_edit_gpu_contract/creative_detail_stage_contract_test.cpp
        tests/warm_edit_gpu_contract/selective_tone_stage_contract_test.cpp
        tests/warm_edit_gpu_contract/composed_stage_contract_test.cpp
        tests/warm_edit_gpu_contract/layer_composition_contract_test.cpp
        tests/warm_edit_gpu_contract/mask_coverage_contract_test.cpp
        tests/warm_edit_gpu_contract/retouch_stage_contract_test.cpp
        tests/warm_edit_gpu_contract/geometry_stage_contract_test.cpp
        tests/warm_edit_gpu_contract/resident_resource_cache_contract_test.cpp
    )
    target_link_libraries(
        shadow-image-warm-edit-gpu-tests
        PRIVATE Shadow::Image
    )
    shadow_image_enable_warnings(shadow-image-warm-edit-gpu-tests)
    add_test(
        NAME shadow-image-warm-edit-gpu-contract
        COMMAND shadow-image-warm-edit-gpu-tests
    )

    shadow_image_add_contract(
        shadow-image-detail-tile-session-tests
        shadow-image-detail-tile-session-contract
        tests/detail_tile_session_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-detail-tile-display-output-tests
        shadow-image-detail-tile-display-output-contract
        tests/detail_tile_display_output_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-detail-tile-seam-tests
        shadow-image-detail-tile-seam-contract
        tests/detail_tile_seam_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-detail-tile-layer-seam-tests
        shadow-image-detail-tile-layer-seam-contract
        tests/detail_tile_layer_seam_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-detail-tile-retouch-seam-tests
        shadow-image-detail-tile-retouch-seam-contract
        tests/detail_tile_retouch_seam_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-detail-tile-geometry-seam-tests
        shadow-image-detail-tile-geometry-seam-contract
        tests/detail_tile_geometry_seam_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-detail-tile-validation-tests
        shadow-image-detail-tile-validation-contract
        tests/detail_tile_validation_contract_test.cpp
    )

    add_executable(shadow-image-display-luma-tests tests/display_luma_contract_test.cpp)
    target_link_libraries(shadow-image-display-luma-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-display-luma-tests)
    add_test(NAME shadow-image-display-luma-contract COMMAND shadow-image-display-luma-tests)

    add_executable(
        shadow-image-display-output-tests
        tests/display_output_contract_test.cpp
    )
    target_link_libraries(shadow-image-display-output-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-display-output-tests)
    add_test(
        NAME shadow-image-display-output-contract
        COMMAND shadow-image-display-output-tests
    )

    add_executable(shadow-image-lut-tests tests/lut_contract_test.cpp)
    target_link_libraries(shadow-image-lut-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-lut-tests)
    add_test(NAME shadow-image-lut-contract COMMAND shadow-image-lut-tests)

    add_executable(shadow-image-source-rendering-tests tests/source_rendering_contract_test.cpp)
    target_link_libraries(shadow-image-source-rendering-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-source-rendering-tests)
    add_test(NAME shadow-image-source-rendering-contract COMMAND shadow-image-source-rendering-tests)

    shadow_image_add_contract(
        shadow-image-raw-pipeline-routing-tests
        shadow-image-raw-pipeline-routing-contract
        tests/raw_pipeline_routing_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-raw-sensor-preparation-tests
        shadow-image-raw-sensor-preparation-contract
        tests/raw_sensor_preparation_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-neural-raw-denoise-tests
        shadow-image-neural-raw-denoise-contract
        tests/neural_raw_denoise_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-raw-foundation-tests
        shadow-image-raw-foundation-contract
        tests/raw_foundation_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-raw-foundation-source-route-tests
        shadow-image-raw-foundation-source-route-contract
        tests/raw_foundation_source_route_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-raw-foundation-edit-surfaces-tests
        shadow-image-raw-foundation-edit-surfaces-contract
        tests/raw_foundation_edit_surfaces_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-raw-frame-development-plan-tests
        shadow-image-raw-frame-development-plan-contract
        tests/raw_frame_development_plan_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-raw-frame-staging-tests
        shadow-image-raw-frame-staging-contract
        tests/raw_frame_staging_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-raw-white-balance-tests
        shadow-image-raw-white-balance-contract
        tests/raw_white_balance_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-raw-preview-rebinding-tests
        shadow-image-raw-preview-rebinding-contract
        tests/raw_preview_rebinding_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-resident-raw-source-tests
        shadow-image-resident-raw-source-contract
        tests/resident_raw_source_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-metal-resident-raw-source-tests
        shadow-image-metal-resident-raw-source-contract
        tests/metal_resident_raw_source_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-full-edit-detail-metal-raw-tests
        shadow-image-full-edit-detail-metal-raw-contract
        tests/full_edit_detail_metal_raw_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-metal-scene-linear-region-optics-tests
        shadow-image-metal-scene-linear-region-optics-contract
        tests/metal_scene_linear_region_optics_contract_test.cpp
    )
    if(APPLE AND SHADOW_ENABLE_METAL)
        set(
            SHADOW_METAL_SCENE_LINEAR_REGION_OPTICS_TEST_ENVIRONMENT
            "SHADOW_TEST_REQUIRE_METAL=1"
        )
        if(SHADOW_LENSFUN_TEST_DATABASE)
            list(
                APPEND SHADOW_METAL_SCENE_LINEAR_REGION_OPTICS_TEST_ENVIRONMENT
                "SHADOW_TEST_LENSFUN_DB=$<SHELL_PATH:${SHADOW_LENSFUN_TEST_DATABASE}>"
            )
        endif()
        set_tests_properties(
            shadow-image-metal-scene-linear-region-optics-contract
            PROPERTIES
                ENVIRONMENT "${SHADOW_METAL_SCENE_LINEAR_REGION_OPTICS_TEST_ENVIRONMENT}"
                LABELS "metal-required"
        )
    endif()

    shadow_image_add_contract(
        shadow-image-fused-raw-cpu-development-tests
        shadow-image-fused-raw-cpu-development-contract
        tests/fused_raw_cpu_development_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-fused-raw-metal-execution-tests
        shadow-image-fused-raw-metal-execution-contract
        tests/fused_raw_metal_execution_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-fused-raw-highlight-treatment-tests
        shadow-image-fused-raw-highlight-treatment-contract
        tests/fused_raw_highlight_treatment_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-fused-raw-input-validation-tests
        shadow-image-fused-raw-input-validation-contract
        tests/fused_raw_input_validation_contract_test.cpp
    )
    if(SHADOW_REQUIRE_METAL_TESTS)
        set_tests_properties(
            shadow-image-edit-preview-layer-execution-contract
            shadow-image-warm-edit-gpu-contract
            shadow-image-detail-tile-layer-seam-contract
            shadow-image-detail-tile-retouch-seam-contract
            shadow-image-detail-tile-geometry-seam-contract
            PROPERTIES
                ENVIRONMENT
                    "SHADOW_IMAGE_ACCELERATION=metal;SHADOW_TEST_REQUIRE_WARM_METAL=1"
                LABELS "metal-required"
        )
#This contract intentionally exercises both automatic all - or -nothing CPU replay and a
#scoped forced - Metal failure.Require a real device without overriding its outer
#automatic - selection premise.
        set_tests_properties(
            shadow-image-edit-preview-execution-contract
            PROPERTIES
                ENVIRONMENT
                    "SHADOW_IMAGE_ACCELERATION=auto;SHADOW_TEST_REQUIRE_WARM_METAL=1"
                LABELS "metal-required"
        )
        set_tests_properties(
            shadow-image-edit-preview-geometry-execution-contract
            PROPERTIES
                ENVIRONMENT "SHADOW_TEST_REQUIRE_WARM_METAL=1"
                LABELS "metal-required"
        )
        set_tests_properties(
            shadow-image-adjustment-execution-contract
            PROPERTIES
                ENVIRONMENT
                    "SHADOW_IMAGE_ACCELERATION=metal;SHADOW_TEST_REQUIRE_METAL=1"
                LABELS "metal-required"
        )
        set_tests_properties(
            shadow-image-display-output-contract
            PROPERTIES
                ENVIRONMENT
                    "SHADOW_IMAGE_ACCELERATION=metal;SHADOW_TEST_REQUIRE_METAL=1;SHADOW_TEST_METAL_DISPLAY_TILE_BYTES=1"
                LABELS "metal-required"
        )
        set_tests_properties(
            shadow-image-fused-raw-metal-execution-contract
            shadow-image-fused-raw-highlight-treatment-contract
            PROPERTIES
                ENVIRONMENT
                    "SHADOW_IMAGE_ACCELERATION=metal;SHADOW_TEST_REQUIRE_METAL=1;SHADOW_TEST_METAL_TILE_BYTES=1"
                LABELS "metal-required"
        )
        set_tests_properties(
            shadow-image-raw-pipeline-routing-contract
            PROPERTIES
                ENVIRONMENT
                    "SHADOW_IMAGE_ACCELERATION=metal;SHADOW_TEST_REQUIRE_METAL=1"
                LABELS "metal-required"
        )
        set_tests_properties(
            shadow-image-metal-resident-raw-source-contract
            shadow-image-full-edit-detail-metal-raw-contract
            PROPERTIES
                ENVIRONMENT "SHADOW_TEST_REQUIRE_METAL=1"
                LABELS "metal-required"
        )
#This aggregate owns both CPU reference cases and automatic Metal selection for eligible
#denoise, balanced, and high - quality reconstruction.Require a real device without
#overriding those per - case selectors.
        set_tests_properties(
            shadow-image-raw-sensor-preparation-contract
            PROPERTIES
                ENVIRONMENT
                    "SHADOW_IMAGE_ACCELERATION=auto;SHADOW_TEST_REQUIRE_METAL=1"
                LABELS "metal-required"
        )
    endif()

    add_executable(shadow-image-dcp-parser-tests tests/dcp_parser_contract_test.cpp)
    target_link_libraries(shadow-image-dcp-parser-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-dcp-parser-tests)
    add_test(NAME shadow-image-dcp-parser-contract COMMAND shadow-image-dcp-parser-tests)

    shadow_image_add_contract(
        shadow-image-dng-noise-profile-tests
        shadow-image-dng-noise-profile-contract
        tests/dng_noise_profile_contract_test.cpp
    )
    target_include_directories(
        shadow-image-dng-noise-profile-tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder"
    )

    shadow_image_add_contract(
        shadow-image-camera-profile-catalog-tests
        shadow-image-camera-profile-catalog-contract
        tests/camera_profile_catalog_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-dcp-color-transform-tests
        shadow-image-dcp-color-transform-contract
        tests/dcp_color_transform_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-dcp-color-rendering-tests
        shadow-image-dcp-color-rendering-contract
        tests/dcp_color_rendering_contract_test.cpp
    )

    add_executable(
        shadow-image-decode-session-isolation-tests
        tests/decode_session_isolation_contract_test.cpp
    )
    target_link_libraries(shadow-image-decode-session-isolation-tests PRIVATE Shadow::Image)
    target_include_directories(
        shadow-image-decode-session-isolation-tests
        PRIVATE
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    )
    add_dependencies(
        shadow-image-decode-session-isolation-tests
        shadow-image-private-decoder-test-plugin
    )
    target_compile_definitions(
        shadow-image-decode-session-isolation-tests
        PRIVATE
            "SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH=\"$<TARGET_FILE:shadow-image-private-decoder-test-plugin>\""
            "SHADOW_TEST_JPEG_PATH=\"${CMAKE_SOURCE_DIR}/apps/desktop/assets/lut-preview-reference.jpg\""
    )
    shadow_image_enable_warnings(shadow-image-decode-session-isolation-tests)
    add_test(
        NAME shadow-image-decode-session-isolation-contract
        COMMAND shadow-image-decode-session-isolation-tests
    )
endif()

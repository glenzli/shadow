find_package(PkgConfig REQUIRED)
# Parallel source sessions are safe only with LibRaw's reentrant Unix/macOS build.
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

add_library(
    shadow-image STATIC
    src/decoder/libraw_decoder.cpp
    src/decoder/raster_exif.cpp
    src/decoder/raster_decoder.cpp
    src/decoder/heif_decoder.cpp
    src/decoder/decode_session_isolation.cpp
    src/decoder/photo_decoder_router.cpp
    src/color/lcms_color_management.cpp
    src/color/neutral_balance.cpp
    src/color/source_profile_catalog.cpp
    src/color/source_rendering.cpp
    src/concurrency/row_scheduler.cpp
    src/edit/adjustment_execution.cpp
    src/edit/cube_lut.cpp
    src/edit/cpu_reference.cpp
    src/edit/edit_error.cpp
    src/edit/local_mask.cpp
    src/edit/photo_geometry.cpp
    src/edit/retouch.cpp
    src/raw/bayer_demosaic.cpp
    src/raw/bayer_sampling.cpp
    src/raw/camera_profile_catalog.cpp
    src/raw/dcp_color_development.cpp
    src/raw/dcp_parser.cpp
    src/raw/fused_raw_development.cpp
    src/raw/raw_denoise.cpp
    src/raw/raw_pipeline.cpp
    src/raw/sensor_clipping.cpp
    src/decoder/private_decoder_plugin.cpp
    src/optics/lensfun_optics.cpp
    src/proxy/developed_source_raster.cpp
    src/proxy/display_output.cpp
    src/proxy/edited_proxy_rendering.cpp
    src/proxy/full_edit_detail.cpp
    src/proxy/jpeg_display_luma.cpp
    src/proxy/jpeg_proxy_encoding.cpp
    src/proxy/proxy_rendering.cpp
    src/proxy/proxy_render_request_validation.cpp
    src/proxy/warm_edit_preview.cpp
)
if(APPLE AND SHADOW_ENABLE_METAL)
    find_library(SHADOW_METAL_FRAMEWORK Metal REQUIRED)
    find_library(SHADOW_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    target_sources(
        shadow-image
        PRIVATE
            src/edit/metal_adjustment.mm
            src/raw/metal_raw_development.mm
            src/proxy/metal_display_output.mm
            src/proxy/warm_edit_gpu.mm
    )
    target_compile_definitions(shadow-image PRIVATE SHADOW_IMAGE_HAS_METAL=1)
    target_link_libraries(
        shadow-image-platform-runtime
        INTERFACE
            "${SHADOW_METAL_FRAMEWORK}"
            "${SHADOW_FOUNDATION_FRAMEWORK}"
    )
else()
    target_sources(
        shadow-image
        PRIVATE
            src/edit/metal_adjustment_stub.cpp
            src/raw/metal_raw_development_stub.cpp
            src/proxy/metal_display_output_stub.cpp
            src/proxy/warm_edit_gpu_stub.cpp
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
    # An explicitly selected Lensfun prefix must win over broad dependency include roots such as
    # /opt/homebrew/include. Otherwise a developer database build can compile against Homebrew's
    # headers and link a different ABI-compatible-looking library from the selected prefix.
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
    shadow_image_enable_warnings(shadow-raw-probe)
endif()

# The desktop starts this tiny executable for untrusted or proprietary RAW
# decoder work.  A native crash in LibRaw or a locally installed vendor module
# then becomes one failed preview instead of a crashed photo application.
add_executable(shadow-image-decode-helper tools/decode_helper.cpp)
target_link_libraries(shadow-image-decode-helper PRIVATE Shadow::Image)
shadow_image_enable_warnings(shadow-image-decode-helper)

if(BUILD_TESTING)
    # This deliberately contains no vendor SDK. It exercises the same load/create/destroy
    # contract a user's private SDK adapter will use, without allowing public tests to depend
    # on proprietary headers or a real camera file.
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

    # Models an old v1-only artifact that predates the exact interface seal. Its factory aborts;
    # the loader contract test must reject it before construction.
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

    # A public, LibRaw-backed dynamic provider used only to validate the local-provider route.
    # It deliberately resembles the shape of a private vendor plugin without containing one.
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
        shadow-image-decoder-source-tests
        shadow-image-decoder-source-contract
        tests/decoder_source_contract_test.cpp
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
        shadow-image-edit-preview-execution-tests
        shadow-image-edit-preview-execution-contract
        tests/edit_preview_execution_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-libraw-provider-tests
        shadow-image-libraw-provider-contract
        tests/libraw_provider_contract_test.cpp
    )

    shadow_image_add_contract(
        shadow-image-edit-plan-tests
        shadow-image-edit-plan-contract
        tests/edit_plan_contract_test.cpp
    )
    shadow_image_add_contract(
        shadow-image-tone-adjustment-tests
        shadow-image-tone-adjustment-contract
        tests/tone_adjustment_contract_test.cpp
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

    add_executable(shadow-image-detail-tile-tests tests/detail_tile_contract_test.cpp)
    target_link_libraries(shadow-image-detail-tile-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-detail-tile-tests)
    add_test(NAME shadow-image-detail-tile-contract COMMAND shadow-image-detail-tile-tests)

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

    add_executable(shadow-image-raw-pipeline-tests tests/raw_pipeline_contract_test.cpp)
    target_link_libraries(shadow-image-raw-pipeline-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-raw-pipeline-tests)
    add_test(NAME shadow-image-raw-pipeline-contract COMMAND shadow-image-raw-pipeline-tests)

    add_executable(
        shadow-image-fused-raw-development-tests
        tests/fused_raw_development_contract_test.cpp
    )
    target_link_libraries(shadow-image-fused-raw-development-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-fused-raw-development-tests)
    add_test(
        NAME shadow-image-fused-raw-development-contract
        COMMAND shadow-image-fused-raw-development-tests
    )
    if(SHADOW_REQUIRE_METAL_TESTS)
        set_tests_properties(
            shadow-image-edit-preview-execution-contract
            PROPERTIES
                ENVIRONMENT
                    "SHADOW_IMAGE_ACCELERATION=metal;SHADOW_TEST_REQUIRE_WARM_METAL=1"
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
            shadow-image-fused-raw-development-contract
            PROPERTIES
                ENVIRONMENT
                    "SHADOW_IMAGE_ACCELERATION=metal;SHADOW_TEST_REQUIRE_METAL=1;SHADOW_TEST_METAL_TILE_BYTES=1"
                LABELS "metal-required"
        )
        set_tests_properties(
            shadow-image-raw-pipeline-contract
            PROPERTIES
                ENVIRONMENT
                    "SHADOW_IMAGE_ACCELERATION=metal;SHADOW_TEST_REQUIRE_METAL=1"
                LABELS "metal-required"
        )
        set_tests_properties(
            shadow-image-raw-pipeline-contract
            PROPERTIES
                ENVIRONMENT "SHADOW_IMAGE_ACCELERATION=metal"
                LABELS "metal-required"
        )
    endif()

    add_executable(shadow-image-dcp-parser-tests tests/dcp_parser_contract_test.cpp)
    target_link_libraries(shadow-image-dcp-parser-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-dcp-parser-tests)
    add_test(NAME shadow-image-dcp-parser-contract COMMAND shadow-image-dcp-parser-tests)

    add_executable(
        shadow-image-camera-profile-tests
        tests/camera_profile_contract_test.cpp
    )
    target_link_libraries(shadow-image-camera-profile-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-camera-profile-tests)
    add_test(
        NAME shadow-image-camera-profile-contract
        COMMAND shadow-image-camera-profile-tests
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

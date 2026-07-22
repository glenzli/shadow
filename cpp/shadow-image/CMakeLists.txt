find_package(PkgConfig REQUIRED)
pkg_check_modules(LibRaw REQUIRED IMPORTED_TARGET libraw)
pkg_check_modules(Lensfun QUIET IMPORTED_TARGET lensfun)
pkg_check_modules(LittleCms REQUIRED IMPORTED_TARGET lcms2)
find_package(JPEG REQUIRED)

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
    src/color/lcms_color_management.cpp
    src/edit/cube_lut.cpp
    src/edit/cpu_reference.cpp
    src/decoder/private_decoder_plugin.cpp
    src/optics/lensfun_optics.cpp
    src/proxy/jpeg_display_luma.cpp
    src/proxy/jpeg_proxy.cpp
)
add_library(Shadow::Image ALIAS shadow-image)
target_compile_features(shadow-image PUBLIC cxx_std_20)
target_include_directories(
    shadow-image
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    shadow-image
    PRIVATE PkgConfig::LibRaw PkgConfig::LittleCms JPEG::JPEG ${CMAKE_DL_LIBS}
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
shadow_image_enable_warnings(shadow-image)

if(SHADOW_BUILD_RAW_PROBE)
    add_executable(shadow-raw-probe tools/raw_probe.cpp)
    target_link_libraries(shadow-raw-probe PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-raw-probe)
endif()

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

    add_executable(shadow-image-contract-tests tests/decoder_contract_test.cpp)
    target_link_libraries(shadow-image-contract-tests PRIVATE Shadow::Image)
    add_dependencies(shadow-image-contract-tests shadow-image-private-decoder-test-plugin)
    target_compile_definitions(
        shadow-image-contract-tests
        PRIVATE
            "SHADOW_TEST_PRIVATE_DECODER_PLUGIN_PATH=\"$<TARGET_FILE:shadow-image-private-decoder-test-plugin>\""
    )
    shadow_image_enable_warnings(shadow-image-contract-tests)
    add_test(NAME shadow-image-contract COMMAND shadow-image-contract-tests)
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
                shadow-image-contract
                PROPERTIES ENVIRONMENT
                    "SHADOW_TEST_LENSFUN_DB=$<SHELL_PATH:${SHADOW_LENSFUN_TEST_DATABASE}>"
            )
        endif()
    endif()

    add_executable(shadow-image-edit-tests tests/edit_contract_test.cpp)
    target_link_libraries(shadow-image-edit-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-edit-tests)
    add_test(NAME shadow-image-edit-contract COMMAND shadow-image-edit-tests)

    add_executable(shadow-image-detail-tile-tests tests/detail_tile_contract_test.cpp)
    target_link_libraries(shadow-image-detail-tile-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-detail-tile-tests)
    add_test(NAME shadow-image-detail-tile-contract COMMAND shadow-image-detail-tile-tests)

    add_executable(shadow-image-display-luma-tests tests/display_luma_contract_test.cpp)
    target_link_libraries(shadow-image-display-luma-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-display-luma-tests)
    add_test(NAME shadow-image-display-luma-contract COMMAND shadow-image-display-luma-tests)

    add_executable(shadow-image-lut-tests tests/lut_contract_test.cpp)
    target_link_libraries(shadow-image-lut-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-lut-tests)
    add_test(NAME shadow-image-lut-contract COMMAND shadow-image-lut-tests)
endif()

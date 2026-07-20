find_package(PkgConfig REQUIRED)
pkg_check_modules(LibRaw REQUIRED IMPORTED_TARGET libraw)
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
    src/edit/cpu_reference.cpp
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
target_link_libraries(shadow-image PRIVATE PkgConfig::LibRaw JPEG::JPEG)
shadow_image_enable_warnings(shadow-image)

if(SHADOW_BUILD_RAW_PROBE)
    add_executable(shadow-raw-probe tools/raw_probe.cpp)
    target_link_libraries(shadow-raw-probe PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-raw-probe)
endif()

if(BUILD_TESTING)
    add_executable(shadow-image-contract-tests tests/decoder_contract_test.cpp)
    target_link_libraries(shadow-image-contract-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-contract-tests)
    add_test(NAME shadow-image-contract COMMAND shadow-image-contract-tests)

    add_executable(shadow-image-edit-tests tests/edit_contract_test.cpp)
    target_link_libraries(shadow-image-edit-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-edit-tests)
    add_test(NAME shadow-image-edit-contract COMMAND shadow-image-edit-tests)

    add_executable(shadow-image-display-luma-tests tests/display_luma_contract_test.cpp)
    target_link_libraries(shadow-image-display-luma-tests PRIVATE Shadow::Image)
    shadow_image_enable_warnings(shadow-image-display-luma-tests)
    add_test(NAME shadow-image-display-luma-contract COMMAND shadow-image-display-luma-tests)
endif()

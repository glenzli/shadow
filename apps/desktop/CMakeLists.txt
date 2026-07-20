find_package(Qt6 6.8 REQUIRED COMPONENTS Concurrent Quick QuickControls2)
find_package(PkgConfig REQUIRED)
find_package(JPEG REQUIRED)
find_package(Iconv REQUIRED)
pkg_check_modules(LibRaw REQUIRED IMPORTED_TARGET libraw)

qt_standard_project_setup(REQUIRES 6.8)

find_program(SHADOW_CARGO_EXECUTABLE cargo REQUIRED)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(SHADOW_CARGO_PROFILE release)
    set(SHADOW_CARGO_PROFILE_ARGUMENT --release)
else()
    set(SHADOW_CARGO_PROFILE debug)
    set(SHADOW_CARGO_PROFILE_ARGUMENT)
endif()

file(
    GLOB_RECURSE SHADOW_RUST_BRIDGE_INPUTS
    CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/Cargo.toml"
    "${PROJECT_SOURCE_DIR}/Cargo.lock"
    "${PROJECT_SOURCE_DIR}/crates/*/Cargo.toml"
    "${PROJECT_SOURCE_DIR}/crates/*/src/*.rs"
    "${PROJECT_SOURCE_DIR}/crates/*/src/**/*.rs"
)

set(
    SHADOW_DESKTOP_RUST_LIBRARY
    "${PROJECT_SOURCE_DIR}/target/${SHADOW_CARGO_PROFILE}/libshadow_desktop_bridge.a"
)
set(SHADOW_CARGO_ENV "${CMAKE_COMMAND}" -E env)
if(APPLE)
    # Homebrew's Qt 6.11 frameworks currently target macOS 26.0. Match the
    # Rust static library so the final app does not raise mixed-minimum warnings.
    list(APPEND SHADOW_CARGO_ENV MACOSX_DEPLOYMENT_TARGET=26.0)
endif()
add_custom_command(
    OUTPUT "${SHADOW_DESKTOP_RUST_LIBRARY}"
    COMMAND
        ${SHADOW_CARGO_ENV} "${SHADOW_CARGO_EXECUTABLE}" build
        --package shadow-desktop-bridge
        ${SHADOW_CARGO_PROFILE_ARGUMENT}
        --target-dir "${PROJECT_SOURCE_DIR}/target"
    DEPENDS ${SHADOW_RUST_BRIDGE_INPUTS}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Building the Rust desktop snapshot bridge"
    VERBATIM
)
add_custom_target(shadow-desktop-rust-build DEPENDS "${SHADOW_DESKTOP_RUST_LIBRARY}")
add_library(shadow-desktop-rust STATIC IMPORTED GLOBAL)
set_target_properties(
    shadow-desktop-rust
    PROPERTIES IMPORTED_LOCATION "${SHADOW_DESKTOP_RUST_LIBRARY}"
)
add_dependencies(shadow-desktop-rust shadow-desktop-rust-build)

qt_add_executable(
    shadow-desktop
    MACOSX_BUNDLE
    src/desktop_backend.cpp
    src/desktop_backend.hpp
    src/main.cpp
    src/review_controller.cpp
    src/review_controller.hpp
    src/review_model.cpp
    src/review_model.hpp
    src/thumbnail_provider.cpp
    src/thumbnail_provider.hpp
)

qt_add_qml_module(
    shadow-desktop
    URI Shadow.App
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_compile_features(shadow-desktop PRIVATE cxx_std_20)
target_include_directories(shadow-desktop PRIVATE "${PROJECT_SOURCE_DIR}/target/cxxbridge")
target_link_libraries(
    shadow-desktop
    PRIVATE
        shadow-desktop-rust
        PkgConfig::LibRaw
        JPEG::JPEG
        Iconv::Iconv
        Qt6::Concurrent
        Qt6::Quick
        Qt6::QuickControls2
)

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

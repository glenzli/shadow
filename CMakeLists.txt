cmake_minimum_required(VERSION 3.28)

project(
    Shadow
    VERSION 0.1.0
    DESCRIPTION "Local-first photo catalog and non-destructive editor"
    LANGUAGES CXX
)

option(SHADOW_BUILD_RAW_PROBE "Build the LibRaw vertical-spike probe" ON)
option(SHADOW_BUILD_DESKTOP "Build the Qt Quick desktop application" OFF)
option(SHADOW_ENABLE_METAL "Build the macOS Metal image backend" ${APPLE})
option(
    SHADOW_REQUIRE_METAL_TESTS
    "Require native tests to execute the Metal backend instead of allowing a local skip"
    OFF
)

if(SHADOW_ENABLE_METAL AND NOT APPLE)
    message(FATAL_ERROR "SHADOW_ENABLE_METAL is available only on macOS")
endif()
if(SHADOW_REQUIRE_METAL_TESTS AND (NOT APPLE OR NOT SHADOW_ENABLE_METAL))
    message(FATAL_ERROR
        "SHADOW_REQUIRE_METAL_TESTS requires macOS with SHADOW_ENABLE_METAL=ON"
    )
endif()

if(APPLE)
    # shadow-image is configured before the desktop target, so Objective-C++ must be available at
    # the project boundary rather than enabled later by the Qt subdirectory.
    enable_language(OBJCXX)
endif()

# Lensfun remains an optional open dependency: Shadow may build a safe no-profile adapter when it
# is absent, but a real optical pipeline needs both its library and calibration database. A local
# developer, packager, or CI image can point this cache variable at a normal installation prefix
# without teaching every nested CMake/Cargo build about a machine-specific pkg-config path.
set(
    SHADOW_LENSFUN_ROOT
    ""
    CACHE PATH
    "Optional Lensfun installation prefix (contains include/, lib/, and share/lensfun/)"
)
set(SHADOW_LENSFUN_PKGCONFIG_PATH "")
if(SHADOW_LENSFUN_ROOT)
    set(SHADOW_LENSFUN_PKGCONFIG_PATH "${SHADOW_LENSFUN_ROOT}/lib/pkgconfig")
    if(NOT EXISTS "${SHADOW_LENSFUN_PKGCONFIG_PATH}")
        message(FATAL_ERROR
            "SHADOW_LENSFUN_ROOT must contain lib/pkgconfig/lensfun.pc: ${SHADOW_LENSFUN_ROOT}"
        )
    endif()
    list(PREPEND CMAKE_PREFIX_PATH "${SHADOW_LENSFUN_ROOT}")
    if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
        set(
            ENV{PKG_CONFIG_PATH}
            "${SHADOW_LENSFUN_PKGCONFIG_PATH}:$ENV{PKG_CONFIG_PATH}"
        )
    else()
        set(ENV{PKG_CONFIG_PATH} "${SHADOW_LENSFUN_PKGCONFIG_PATH}")
    endif()
endif()

include(CTest)
add_subdirectory(cpp/shadow-image)

if(SHADOW_BUILD_DESKTOP)
    add_subdirectory(apps/desktop)
endif()

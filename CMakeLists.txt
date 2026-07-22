cmake_minimum_required(VERSION 3.28)

project(
    Shadow
    VERSION 0.1.0
    DESCRIPTION "Local-first photo catalog and non-destructive editor"
    LANGUAGES CXX
)

option(SHADOW_BUILD_RAW_PROBE "Build the LibRaw vertical-spike probe" ON)
option(SHADOW_BUILD_DESKTOP "Build the Qt Quick desktop application" OFF)

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

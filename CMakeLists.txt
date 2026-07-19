cmake_minimum_required(VERSION 3.28)

project(
    Shadow
    VERSION 0.1.0
    DESCRIPTION "Local-first photo catalog and non-destructive editor"
    LANGUAGES CXX
)

option(SHADOW_BUILD_RAW_PROBE "Build the LibRaw vertical-spike probe" ON)

if(SHADOW_BUILD_RAW_PROBE)
    add_subdirectory(cpp/shadow-image)
endif()

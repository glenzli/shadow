# Cargo's cxx build integration publishes headers below a hash-qualified
# OUT_DIR. CMake needs a stable include root for the desktop compilation, so
# copy the one package-owned generated tree after Cargo succeeds. This operates
# only inside the current CMake build directory.

foreach(variable IN ITEMS
    SHADOW_CARGO_TARGET_DIRECTORY
    SHADOW_CARGO_PROFILE
    SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY
)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "sync_cxxbridge_headers requires ${variable}")
    endif()
endforeach()

if(DEFINED SHADOW_CXXBRIDGE_OUT_DIR_RECORD AND NOT "${SHADOW_CXXBRIDGE_OUT_DIR_RECORD}" STREQUAL "")
    if(NOT EXISTS "${SHADOW_CXXBRIDGE_OUT_DIR_RECORD}")
        message(FATAL_ERROR "Cargo OUT_DIR record is missing")
    endif()
    file(READ "${SHADOW_CXXBRIDGE_OUT_DIR_RECORD}" registered_out_directory)
    string(STRIP "${registered_out_directory}" registered_out_directory)
    file(REAL_PATH "${registered_out_directory}" registered_out_directory)
    file(REAL_PATH "${SHADOW_CARGO_TARGET_DIRECTORY}/${SHADOW_CARGO_PROFILE}/build" expected_build_directory)
    cmake_path(IS_PREFIX expected_build_directory "${registered_out_directory}" NORMALIZE registered_inside_build)
    if(NOT registered_inside_build OR NOT registered_out_directory MATCHES "/shadow-desktop-bridge-[^/]+/out$")
        message(FATAL_ERROR "Cargo OUT_DIR record is outside the current desktop build")
    endif()
    set(candidate_directories "${registered_out_directory}/cxxbridge/include")
else()
    file(
        GLOB candidate_directories
        LIST_DIRECTORIES true
        "${SHADOW_CARGO_TARGET_DIRECTORY}/${SHADOW_CARGO_PROFILE}/build/shadow-desktop-bridge-*/out/cxxbridge/include"
    )
endif()
set(valid_directories)
foreach(candidate_directory IN LISTS candidate_directories)
    if(EXISTS "${candidate_directory}/shadow-desktop-bridge/src/lib.rs.h")
        list(APPEND valid_directories "${candidate_directory}")
    endif()
endforeach()
list(LENGTH valid_directories valid_directory_count)
if(NOT valid_directory_count EQUAL 1)
    message(FATAL_ERROR
        "expected one generated shadow-desktop-bridge cxxbridge include directory, found ${valid_directory_count}"
    )
endif()

list(GET valid_directories 0 generated_include_directory)
file(MAKE_DIRECTORY "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}")
if(WIN32)
    file(GLOB_RECURSE generated_headers LIST_DIRECTORIES false
        RELATIVE "${generated_include_directory}" "${generated_include_directory}/*")
    foreach(header IN LISTS generated_headers)
        cmake_path(GET header PARENT_PATH header_directory)
        file(MAKE_DIRECTORY "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}/${header_directory}")
        file(COPY_FILE "${generated_include_directory}/${header}"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}/${header}" ONLY_IF_DIFFERENT)
    endforeach()
else()
    file(COPY "${generated_include_directory}/" DESTINATION "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}")
endif()
message(STATUS "Synced Cargo cxxbridge headers into ${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}")

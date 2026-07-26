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

file(
    GLOB candidate_directories
    LIST_DIRECTORIES true
    "${SHADOW_CARGO_TARGET_DIRECTORY}/${SHADOW_CARGO_PROFILE}/build/shadow-desktop-bridge-*/out/cxxbridge/include"
)
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
file(COPY "${generated_include_directory}/" DESTINATION "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}")
message(STATUS "Synced Cargo cxxbridge headers into ${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}")

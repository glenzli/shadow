if(NOT DEFINED SHADOW_RAWNIND_SOURCE_RUNTIME)
    message(FATAL_ERROR "SHADOW_RAWNIND_SOURCE_RUNTIME is required")
endif()
if(NOT DEFINED SHADOW_RAWNIND_DESTINATION_RUNTIME)
    message(FATAL_ERROR "SHADOW_RAWNIND_DESTINATION_RUNTIME is required")
endif()
if(NOT IS_DIRECTORY "${SHADOW_RAWNIND_SOURCE_RUNTIME}")
    message(FATAL_ERROR
        "RawNIND source runtime is not a directory: "
        "${SHADOW_RAWNIND_SOURCE_RUNTIME}"
    )
endif()

cmake_path(
    GET SHADOW_RAWNIND_SOURCE_RUNTIME
    FILENAME SHADOW_RAWNIND_SOURCE_RUNTIME_NAME
)
cmake_path(
    GET SHADOW_RAWNIND_DESTINATION_RUNTIME
    FILENAME SHADOW_RAWNIND_DESTINATION_RUNTIME_NAME
)
if(
    NOT SHADOW_RAWNIND_SOURCE_RUNTIME_NAME STREQUAL "_rawnind_runtime"
    OR NOT SHADOW_RAWNIND_DESTINATION_RUNTIME_NAME STREQUAL "_rawnind_runtime"
)
    message(FATAL_ERROR
        "RawNIND runtime installation is restricted to _rawnind_runtime directories"
    )
endif()

# PyInstaller onedir bundles use relative symlinks on POSIX. `cmake -E
# copy_directory` dereferences those links, inflating the app and potentially
# changing runtime lookup behavior. `file(COPY)` preserves the link topology.
file(REMOVE_RECURSE "${SHADOW_RAWNIND_DESTINATION_RUNTIME}")
file(MAKE_DIRECTORY "${SHADOW_RAWNIND_DESTINATION_RUNTIME}")
file(
    COPY "${SHADOW_RAWNIND_SOURCE_RUNTIME}/"
    DESTINATION "${SHADOW_RAWNIND_DESTINATION_RUNTIME}"
    USE_SOURCE_PERMISSIONS
)

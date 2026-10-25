# Capture the exact Cargo build-script artifact on Windows. SDK/feature updates
# can leave several valid OUT_DIRs; directory order or mtime is not ABI evidence.
foreach(variable IN ITEMS SHADOW_CARGO_EXECUTABLE SHADOW_CARGO_TARGET_DIRECTORY
        SHADOW_CARGO_PROFILE SHADOW_CARGO_WORKSPACE_DIRECTORY SHADOW_CXXBRIDGE_OUT_DIR_RECORD)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "build_windows_rust_bridge requires ${variable}")
    endif()
endforeach()
set(profile_arguments)
if(SHADOW_CARGO_PROFILE STREQUAL "release")
    list(APPEND profile_arguments --release)
elseif(NOT SHADOW_CARGO_PROFILE STREQUAL "debug")
    message(FATAL_ERROR "unsupported desktop Cargo profile")
endif()
file(MAKE_DIRECTORY "${SHADOW_CARGO_TARGET_DIRECTORY}")
set(messages "${SHADOW_CARGO_TARGET_DIRECTORY}/desktop-cargo-messages.jsonl")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env SHADOW_BRIDGE_EXTERNAL_IMAGE=1 SHADOW_ENABLE_METAL=0
        "${SHADOW_CARGO_EXECUTABLE}" build --package shadow-desktop-bridge
        ${profile_arguments} --target-dir "${SHADOW_CARGO_TARGET_DIRECTORY}"
        --locked --message-format=json-render-diagnostics
    WORKING_DIRECTORY "${SHADOW_CARGO_WORKSPACE_DIRECTORY}"
    OUTPUT_FILE "${messages}"
    RESULT_VARIABLE build_result)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "Rust desktop bridge build failed: ${build_result}")
endif()
file(STRINGS "${messages}" cargo_messages ENCODING UTF-8)
set(out_directories)
foreach(message IN LISTS cargo_messages)
    string(JSON reason ERROR_VARIABLE parse_error GET "${message}" reason)
    if(parse_error OR NOT reason STREQUAL "build-script-executed")
        continue()
    endif()
    string(JSON package GET "${message}" package_id)
    if(package MATCHES "/shadow-desktop-bridge#")
        string(JSON out_directory GET "${message}" out_dir)
        list(APPEND out_directories "${out_directory}")
    endif()
endforeach()
list(REMOVE_DUPLICATES out_directories)
list(LENGTH out_directories out_count)
if(NOT out_count EQUAL 1)
    message(FATAL_ERROR "Cargo did not identify one current desktop bridge OUT_DIR: ${out_count}")
endif()
list(GET out_directories 0 out_directory)
file(WRITE "${SHADOW_CXXBRIDGE_OUT_DIR_RECORD}.partial" "${out_directory}\n")
file(RENAME "${SHADOW_CXXBRIDGE_OUT_DIR_RECORD}.partial" "${SHADOW_CXXBRIDGE_OUT_DIR_RECORD}")

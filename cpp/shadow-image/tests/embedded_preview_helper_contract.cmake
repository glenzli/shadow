if(NOT EXISTS "${DECODE_HELPER}" OR NOT EXISTS "${PRIVATE_PROVIDER}")
    message(FATAL_ERROR "source-fresh helper and provider fixture are required")
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(root "${TEST_OUTPUT_ROOT}/embedded-preview-${run_identity}")
file(MAKE_DIRECTORY "${root}")
set(nonce "019f99ae-1234-4abc-8def-0123456789ab")

foreach(case IN ITEMS camera-preview camera-preview-portrait camera-preview-portrait-16 no-preview)
    set(source "${root}/${case}.raw")
    set(output "${root}/${case}.jpg")
    file(WRITE "${source}" "independent read-only decoder fixture")
    file(SHA256 "${source}" before)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env
        "SHADOW_DISABLE_PRIVATE_DECODER=0"
        "SHADOW_PRIVATE_DECODER_PLUGIN_PATH=${PRIVATE_PROVIDER}"
        "${DECODE_HELPER}" embedded-preview "${source}" "${output}" "${nonce}"
        RESULT_VARIABLE status OUTPUT_VARIABLE response ERROR_VARIABLE diagnostic
        OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 20)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "camera preview helper failed: ${status}: ${diagnostic}")
    endif()
    file(SHA256 "${source}" after)
    if(NOT before STREQUAL after)
        message(FATAL_ERROR "embedded preview operation modified its original")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env
        "SHADOW_DISABLE_PRIVATE_DECODER=0"
        "SHADOW_PRIVATE_DECODER_PLUGIN_PATH=${PRIVATE_PROVIDER}"
        "${DECODE_HELPER}" quick-inspection "${source}" "${output}" "${nonce}"
        RESULT_VARIABLE quick_status OUTPUT_VARIABLE quick_response ERROR_VARIABLE quick_diagnostic
        OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 20)
    if(NOT quick_status EQUAL 0 OR NOT quick_response MATCHES "^shadow-inspect-v5 decoder-snapshot ${nonce} ")
        message(FATAL_ERROR "combined quick inspection lost its metadata frame: ${quick_diagnostic}")
    endif()
    string(FIND "${quick_response}" "${response}" preview_frame_position)
    if(preview_frame_position LESS 0)
        message(FATAL_ERROR "combined quick inspection changed its camera-preview descriptor")
    endif()
    if(case STREQUAL "no-preview")
        if(NOT response STREQUAL "shadow-embedded-preview-v1 ${nonce} none" OR EXISTS "${output}")
            message(FATAL_ERROR "missing camera preview must be an explicit absence without an output")
        endif()
    else()
        if(case STREQUAL "camera-preview")
            set(dimensions "2 1")
        else()
            set(dimensions "1 2")
        endif()
        if(NOT response MATCHES "^shadow-embedded-preview-v1 ${nonce} jpeg 0 ${dimensions} 8 3 [0-9]+$")
            message(FATAL_ERROR "camera bitmap conversion lost its normalized descriptor: ${response}")
        endif()
        file(READ "${output}" header LIMIT 2 HEX)
        if(NOT header STREQUAL "ffd8")
            message(FATAL_ERROR "camera bitmap was not handed off as JPEG")
        endif()
    endif()
endforeach()
file(REMOVE_RECURSE "${root}")

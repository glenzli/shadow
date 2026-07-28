if(NOT DEFINED DECODE_HELPER OR NOT EXISTS "${DECODE_HELPER}")
    message(FATAL_ERROR "DECODE_HELPER must name the built shadow-image-decode-helper")
endif()
if(NOT DEFINED PRIVATE_PROVIDER OR NOT EXISTS "${PRIVATE_PROVIDER}")
    message(FATAL_ERROR "PRIVATE_PROVIDER must name the built private decoder fixture")
endif()
if(NOT DEFINED TEST_OUTPUT_ROOT OR TEST_OUTPUT_ROOT STREQUAL "")
    message(FATAL_ERROR "TEST_OUTPUT_ROOT must name an external contract-artifact directory")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(run_directory "${TEST_OUTPUT_ROOT}/neutral-detail-${run_identity}")
set(output_path "${run_directory}/detail.rgb")
set(source_path "${run_directory}/fixture.raw")
set(nonce "019f99ae-1234-4abc-8def-0123456789ab")
file(MAKE_DIRECTORY "${run_directory}")

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "SHADOW_DISABLE_PRIVATE_DECODER=0"
        "SHADOW_PRIVATE_DECODER_PLUGIN_PATH=${PRIVATE_PROVIDER}"
        "${DECODE_HELPER}"
        neutral-detail-tile
        "${source_path}"
        "${output_path}"
        0
        0
        2
        1
        "${nonce}"
    RESULT_VARIABLE helper_status
    OUTPUT_VARIABLE helper_stdout
    ERROR_VARIABLE helper_stderr
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    TIMEOUT 30
)
if(NOT helper_status EQUAL 0)
    message(
        FATAL_ERROR
        "neutral detail helper failed (${helper_status})\n"
        "stdout: ${helper_stdout}\n"
        "stderr: ${helper_stderr}"
    )
endif()

string(REGEX REPLACE "[ \t\r\n]+" ";" response_fields "${helper_stdout}")
list(LENGTH response_fields response_field_count)
if(NOT response_field_count EQUAL 13)
    message(
        FATAL_ERROR
        "neutral detail helper returned ${response_field_count} fields instead of 13: "
        "${helper_stdout}"
    )
endif()

function(assert_response_field field_index expected_value field_name)
    list(GET response_fields "${field_index}" actual_value)
    if(NOT actual_value STREQUAL expected_value)
        message(
            FATAL_ERROR
            "neutral detail helper ${field_name} mismatch: "
            "expected '${expected_value}', received '${actual_value}'"
        )
    endif()
endfunction()

assert_response_field(0 "shadow-detail-tile-v1" "protocol")
assert_response_field(1 "neutral-detail-tile" "operation")
assert_response_field(2 "${nonce}" "nonce")
assert_response_field(3 "0000000000000000" "rectangle x")
assert_response_field(4 "0000000000000000" "rectangle y")
assert_response_field(5 "0000000000000002" "rectangle width")
assert_response_field(6 "0000000000000001" "rectangle height")
assert_response_field(7 "0000000000000002" "full width")
assert_response_field(8 "0000000000000001" "full height")
assert_response_field(9 "0000000000000006" "row stride")
assert_response_field(10 "0000000000000006" "byte length")
assert_response_field(11 "0000000000000002" "RAW pipeline path")

list(GET response_fields 12 pipeline_identity)
if(NOT pipeline_identity MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "neutral detail helper returned a non-hex pipeline identity")
endif()
string(LENGTH "${pipeline_identity}" pipeline_identity_length)
math(EXPR pipeline_identity_remainder "${pipeline_identity_length} % 2")
if(pipeline_identity_length EQUAL 0 OR NOT pipeline_identity_remainder EQUAL 0)
    message(FATAL_ERROR "neutral detail helper returned an invalid hex pipeline identity")
endif()

if(NOT EXISTS "${output_path}")
    message(FATAL_ERROR "neutral detail helper did not publish its RGB8 artifact")
endif()
file(SIZE "${output_path}" output_size)
if(NOT output_size EQUAL 6)
    message(
        FATAL_ERROR
        "neutral detail helper artifact must contain exactly 6 bytes, found ${output_size}"
    )
endif()

file(REMOVE_RECURSE "${run_directory}")

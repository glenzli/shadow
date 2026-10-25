# Real model acceptance is explicit: ordinary CI must not download models or spawn Runtime.
option(SHADOW_TEST_INFER_RUNTIME "Run desktop acceptance against an installed local Infer Runtime" OFF)
set(SHADOW_INFER_TEST_PORTRAIT "" CACHE FILEPATH "Explicit public portrait for real desktop AI acceptance")
set(SHADOW_INFER_TEST_RAW "" CACHE FILEPATH "Optional explicit camera RAW for real AI Foundation acceptance")
if(SHADOW_TEST_INFER_RUNTIME)
    if(NOT EXISTS "${SHADOW_INFER_TEST_PORTRAIT}")
        message(FATAL_ERROR "SHADOW_TEST_INFER_RUNTIME requires SHADOW_INFER_TEST_PORTRAIT")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME shadow-desktop-ai-library-runtime
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_library_runtime_integration.py"
            "$<TARGET_FILE:shadow-desktop>" --image "${SHADOW_INFER_TEST_PORTRAIT}"
            --output-base "${CMAKE_CURRENT_BINARY_DIR}/ai-runtime-evidence")
    set_tests_properties(shadow-desktop-ai-library-runtime PROPERTIES
        TIMEOUT 900 RUN_SERIAL TRUE LABELS "desktop;ai;real-runtime")
    foreach(mode ai-subject ai-semantic ai-face ai-completion auto-start subject-emphasis)
        add_test(NAME shadow-desktop-${mode}-runtime
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_editor_runtime_integration.py"
                "$<TARGET_FILE:shadow-desktop>" --image "${SHADOW_INFER_TEST_PORTRAIT}" --mode "${mode}"
                --output-base "${CMAKE_CURRENT_BINARY_DIR}/ai-runtime-evidence")
        set_tests_properties(shadow-desktop-${mode}-runtime PROPERTIES
            TIMEOUT 1900 RUN_SERIAL TRUE LABELS "desktop;ai;real-runtime")
    endforeach()
    if(SHADOW_INFER_TEST_RAW)
        if(NOT EXISTS "${SHADOW_INFER_TEST_RAW}")
            message(FATAL_ERROR "SHADOW_INFER_TEST_RAW must name an existing camera RAW")
        endif()
        add_test(NAME shadow-desktop-ai-raw-foundation-runtime
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_editor_runtime_integration.py"
                "$<TARGET_FILE:shadow-desktop>" --image "${SHADOW_INFER_TEST_RAW}" --mode ai-raw-foundation
                --output-base "${CMAKE_CURRENT_BINARY_DIR}/ai-runtime-evidence")
        set_tests_properties(shadow-desktop-ai-raw-foundation-runtime PROPERTIES
            TIMEOUT 1900 RUN_SERIAL TRUE LABELS "desktop;ai;real-runtime")
    endif()
endif()

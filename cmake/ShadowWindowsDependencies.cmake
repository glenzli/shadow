# Keep UTF-8 source and diagnostic encoding consistent across Qt and native C++ targets.
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()
# MSVC /utf-8 also changes localized /showIncludes diagnostics. CMake's automatic probe can
# decode those bytes using the ANSI code page, losing every header dependency. Match the
# actual UTF-8 compilation mode explicitly; other toolchains retain CMake's normal detection.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    execute_process(COMMAND "${CMAKE_MAKE_PROGRAM}" -t wincodepage
        OUTPUT_VARIABLE shadow_ninja_encoding OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(shadow_ninja_encoding MATCHES "UTF-8")
        set(shadow_probe_directory "${CMAKE_BINARY_DIR}/CMakeFiles/shadow-dependency-probe")
        file(MAKE_DIRECTORY "${shadow_probe_directory}")
        file(WRITE "${shadow_probe_directory}/shadow-dependency-probe.hpp" "// dependency probe\n")
        file(WRITE "${shadow_probe_directory}/probe.cpp" "#include \"shadow-dependency-probe.hpp\"\n")
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /nologo /utf-8 /showIncludes /EP
                "${shadow_probe_directory}/probe.cpp"
            WORKING_DIRECTORY "${shadow_probe_directory}"
            OUTPUT_VARIABLE shadow_probe_output ERROR_VARIABLE shadow_probe_error
            RESULT_VARIABLE shadow_probe_result ENCODING UTF-8)
        if(NOT shadow_probe_result EQUAL 0)
            message(FATAL_ERROR "MSVC dependency-prefix probe failed: ${shadow_probe_error}")
        endif()
        string(REPLACE "\r" "" shadow_probe_output "${shadow_probe_output}\n${shadow_probe_error}")
        string(REPLACE "\n" ";" shadow_probe_lines "${shadow_probe_output}")
        set(shadow_include_prefix_found FALSE)
        foreach(line IN LISTS shadow_probe_lines)
            if(line MATCHES "^(.+: +)[A-Za-z]:[/\\\\].*shadow-dependency-probe\\.hpp$")
                set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_1}")
                set(shadow_include_prefix_found TRUE)
                break()
            endif()
        endforeach()
        if(NOT shadow_include_prefix_found)
            message(FATAL_ERROR "MSVC dependency-prefix probe did not report its included header")
        endif()
    endif()
endif()

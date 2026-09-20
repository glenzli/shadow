if(BUILD_TESTING)
    add_executable(
        shadow-desktop-pipeline-launch
        tests/pipeline_launch_test.cpp
        src/pipeline_launch.cpp
        src/pipeline_launch.hpp
    )
    target_compile_features(shadow-desktop-pipeline-launch PRIVATE cxx_std_20)
    target_include_directories(shadow-desktop-pipeline-launch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(shadow-desktop-pipeline-launch PRIVATE Qt6::Core shadow-desktop-export-backend)
    add_test(NAME shadow-desktop-pipeline-launch COMMAND shadow-desktop-pipeline-launch)
endif()

if(BUILD_TESTING)
    add_executable(shadow-desktop-pipeline-session tests/pipeline_session_test.cpp)
    target_compile_features(shadow-desktop-pipeline-session PRIVATE cxx_std_20)
    target_link_libraries(shadow-desktop-pipeline-session PRIVATE Qt6::Core Qt6::Gui)
    add_dependencies(shadow-desktop-pipeline-session shadow-desktop)
    foreach(scenario legacy batch cancel interactive unsupported)
        add_test(NAME shadow-desktop-pipeline-${scenario}
            COMMAND shadow-desktop-pipeline-session $<TARGET_FILE:shadow-desktop> ${scenario})
        set_tests_properties(shadow-desktop-pipeline-${scenario} PROPERTIES TIMEOUT 120 LABELS "desktop;pipeline")
    endforeach()
endif()

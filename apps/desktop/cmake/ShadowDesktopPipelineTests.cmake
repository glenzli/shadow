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

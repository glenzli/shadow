# Multi-photo registration and fusion stay outside the Qt controller and Rust facade.
find_package(OpenCV REQUIRED COMPONENTS core imgproc photo stitching)
add_library(shadow-photo-composition STATIC
    src/composition/composition_engine.cpp src/composition/composition_engine.hpp)
target_compile_features(shadow-photo-composition PUBLIC cxx_std_20)
target_include_directories(shadow-photo-composition PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/composition" ${OpenCV_INCLUDE_DIRS})
target_link_libraries(shadow-photo-composition PUBLIC ${OpenCV_LIBS})
add_executable(shadow-photo-composition-worker src/composition/composition_worker.cpp)
target_link_libraries(shadow-photo-composition-worker PRIVATE shadow-photo-composition Shadow::Image Qt6::Core Qt6::Gui)
add_dependencies(shadow-desktop shadow-photo-composition-worker)
# A helper-only change must refresh the app's copied helper as well.
set_property(TARGET shadow-desktop APPEND PROPERTY LINK_DEPENDS
    $<TARGET_FILE:shadow-photo-composition-worker>)
add_custom_command(TARGET shadow-desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:shadow-photo-composition-worker>
        $<TARGET_FILE_DIR:shadow-desktop>/$<TARGET_FILE_NAME:shadow-photo-composition-worker>
    COMMENT "Installing the isolated photo composition worker")
if(BUILD_TESTING)
    qt_add_executable(shadow-photo-composition-tests tests/photo_composition_test.cpp src/composition_controller.cpp src/composition_controller.hpp)
    add_dependencies(shadow-photo-composition-tests shadow-photo-composition-worker)
    target_link_libraries(shadow-photo-composition-tests PRIVATE shadow-photo-composition Shadow::Image Qt6::Core Qt6::Test Qt6::Concurrent)
    add_test(NAME shadow-desktop-photo-composition COMMAND shadow-photo-composition-tests)
    set_tests_properties(shadow-desktop-photo-composition PROPERTIES TIMEOUT 90 LABELS "desktop;composition")
endif()

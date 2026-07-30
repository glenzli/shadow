cmake_minimum_required(VERSION 3.25)

project(shadow_sam2_coreml_probe LANGUAGES CXX OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "The SAM 2.1 Core ML probe is available only on Apple platforms")
endif()

add_executable(shadow-sam2-coreml-probe main.mm)
target_compile_features(shadow-sam2-coreml-probe PRIVATE cxx_std_20)
target_compile_options(shadow-sam2-coreml-probe PRIVATE -fobjc-arc -fblocks)
target_link_libraries(
    shadow-sam2-coreml-probe
    PRIVATE
        "-framework CoreML"
        "-framework CoreVideo"
        "-framework Foundation"
)

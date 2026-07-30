cmake_minimum_required(VERSION 3.25)

project(shadow_sam2_coreml_provider LANGUAGES CXX OBJCXX)

if(NOT APPLE)
    message(FATAL_ERROR "The SAM 2.1 Core ML provider is available only on Apple platforms")
endif()

add_executable(
    shadow-sam2-coreml-provider
    main.mm
    resident_server.mm
    sam2_coreml_engine.mm
)
target_compile_features(shadow-sam2-coreml-provider PRIVATE cxx_std_20)
target_compile_options(shadow-sam2-coreml-provider PRIVATE -fobjc-arc -fblocks)
target_link_libraries(
    shadow-sam2-coreml-provider
    PRIVATE
        "-framework CoreGraphics"
        "-framework CoreML"
        "-framework CoreVideo"
        "-framework Foundation"
        "-framework ImageIO"
)

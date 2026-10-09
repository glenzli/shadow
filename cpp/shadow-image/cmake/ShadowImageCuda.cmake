# CUDA owns its compiler and generated shader syntax; the public image API stays portable.
enable_language(CUDA)
find_package(CUDAToolkit 12.0 REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(cuda_source "${CMAKE_CURRENT_SOURCE_DIR}/src/acceleration/cuda")
set(cuda_generated "${CMAKE_CURRENT_BINARY_DIR}/cuda-generated/cuda_shared_math.cuh")
add_custom_command(OUTPUT "${cuda_generated}"
    COMMAND "${Python3_EXECUTABLE}" "${cuda_source}/generate_shaders.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${cuda_generated}"
    DEPENDS "${cuda_source}/generate_shaders.py"
        src/edit/metal_adjustment_msl.hpp src/proxy/warm_edit_gpu_msl.hpp
    VERBATIM)
set(cuda_device_sources ${SHADOW_IMAGE_CUDA_SOURCES})
list(FILTER cuda_device_sources INCLUDE REGEX "\\.cu$")
set(cuda_host_sources ${SHADOW_IMAGE_CUDA_SOURCES})
list(FILTER cuda_host_sources INCLUDE REGEX "\\.cpp$")
add_library(shadow-image-cuda OBJECT ${cuda_device_sources} "${cuda_generated}")
set_target_properties(shadow-image-cuda PROPERTIES
    CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_include_directories(shadow-image-cuda PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include" "${cuda_source}"
    "${CMAKE_CURRENT_BINARY_DIR}/cuda-generated")
# Avoid contraction changing the shared interpreter's operation boundaries.
target_compile_options(shadow-image-cuda PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false>)
target_link_libraries(shadow-image-cuda PUBLIC CUDA::cudart_static)
target_sources(shadow-image PRIVATE ${cuda_host_sources})
target_compile_definitions(shadow-image PRIVATE SHADOW_IMAGE_HAS_CUDA=1)
target_link_libraries(shadow-image PUBLIC shadow-image-cuda)

if(BUILD_TESTING)
    add_executable(shadow-image-cuda-execution-tests
        tests/cuda_image_execution_contract_test.cpp
        tests/warm_edit_gpu_contract/warm_edit_gpu_parity_fixture.cpp)
    target_link_libraries(shadow-image-cuda-execution-tests PRIVATE Shadow::Image)
    add_test(NAME shadow-image-cuda-execution-contract COMMAND shadow-image-cuda-execution-tests)
    set_tests_properties(shadow-image-cuda-execution-contract PROPERTIES LABELS "image;cuda" TIMEOUT 90)
endif()

cmake_minimum_required(VERSION 3.20)
project(ShadowDngSdk VERSION 1.7.1 LANGUAGES CXX)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JXL REQUIRED IMPORTED_TARGET libjxl libjxl_threads)
file(GLOB SDK_SOURCES CONFIGURE_DEPENDS "dng_sdk/source/*.cpp")
# LibRaw uses the read/development SDK. XMP authoring and the standalone validator
# are independent consumers and require the separate XMP Toolkit.
list(FILTER SDK_SOURCES EXCLUDE REGEX "/dng_(validate|xmp_sdk|update_meta)\\.cpp$")
add_library(shadow_dng_sdk STATIC ${SDK_SOURCES} shadow_dng_host.cpp)
target_compile_features(shadow_dng_sdk PUBLIC cxx_std_17)
target_include_directories(shadow_dng_sdk
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/dng_sdk/source"
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
           $<INSTALL_INTERFACE:include>
)
target_compile_definitions(shadow_dng_sdk PRIVATE
    qDNGUseXMP=0 qDNGUseLibJPEG=0 qDNGValidateTarget=0
    _CRT_SECURE_NO_WARNINGS NOMINMAX
)
if(WIN32)
    target_compile_definitions(shadow_dng_sdk PRIVATE qWinOS=1)
elseif(APPLE)
    target_compile_definitions(shadow_dng_sdk PRIVATE qMacOS=1)
else()
    target_compile_definitions(shadow_dng_sdk PRIVATE qLinux=1)
endif()
target_link_libraries(shadow_dng_sdk PRIVATE PkgConfig::JXL)
set_target_properties(shadow_dng_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)
install(TARGETS shadow_dng_sdk EXPORT ShadowDngSdkTargets ARCHIVE DESTINATION lib)
install(FILES shadow_dng_host.h DESTINATION include)
install(DIRECTORY dng_sdk/source/ DESTINATION include/shadow-dng-sdk FILES_MATCHING PATTERN "*.h")
install(EXPORT ShadowDngSdkTargets NAMESPACE ShadowDngSdk:: DESTINATION lib/cmake/ShadowDngSdk)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/shadow-dng-sdk-config.cmake"
    "include(CMakeFindDependencyMacro)\nfind_dependency(PkgConfig)\npkg_check_modules(JXL REQUIRED IMPORTED_TARGET libjxl libjxl_threads)\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/ShadowDngSdkTargets.cmake\")\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/shadow-dng-sdk-config.cmake" DESTINATION lib/cmake/ShadowDngSdk)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/shadow-dng-sdk.pc"
    "prefix=\${pcfiledir}/../..\nlibdir=\${prefix}/lib\nincludedir=\${prefix}/include\nName: shadow-dng-sdk\nDescription: Adobe DNG SDK 1.7.1 with JPEG XL\nVersion: 1.7.1\nLibs: -L\${libdir} -lshadow_dng_sdk\nRequires: libjxl libjxl_threads\nCflags: -I\${includedir}\n")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/shadow-dng-sdk.pc" DESTINATION lib/pkgconfig)

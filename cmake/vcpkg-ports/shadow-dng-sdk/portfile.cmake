vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
vcpkg_download_distfile(ARCHIVE
    URLS "https://download.adobe.com/pub/adobe/dng/dng_sdk_1_7_1.zip"
    FILENAME "shadow-dng-sdk-1.7.1.zip"
    SHA512 3523db4cb6f9d05a54e4df3005f7db5994e5b8e9a6774028161a5d8b457f13c8be8bd090948122d5804cf2308ebdd50f965d81be548ec4d4c045334057a592cd
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}" PATCHES jpeg-xl-api.patch)
file(COPY
    "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
    "${CMAKE_CURRENT_LIST_DIR}/shadow_dng_host.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/shadow_dng_host.h"
    DESTINATION "${SOURCE_PATH}"
)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/ShadowDngSdk")
vcpkg_fixup_pkgconfig()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/DNG_ReadMe.txt")

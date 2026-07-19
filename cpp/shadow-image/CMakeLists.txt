find_package(PkgConfig REQUIRED)
pkg_check_modules(LibRaw REQUIRED IMPORTED_TARGET libraw)

add_executable(shadow-raw-probe tools/raw_probe.cpp)
target_compile_features(shadow-raw-probe PRIVATE cxx_std_20)
target_link_libraries(shadow-raw-probe PRIVATE PkgConfig::LibRaw)

if(MSVC)
    target_compile_options(shadow-raw-probe PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        shadow-raw-probe
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
    )
endif()

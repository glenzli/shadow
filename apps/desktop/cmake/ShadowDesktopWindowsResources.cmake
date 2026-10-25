# Keep generated Windows artwork outside source and share it between both GUI applications.
enable_language(RC)
add_executable(shadow-windows-icon-generator tools/windows_icon.cpp)
set_target_properties(shadow-windows-icon-generator PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_features(shadow-windows-icon-generator PRIVATE cxx_std_20)
target_link_libraries(shadow-windows-icon-generator PRIVATE Qt6::Gui)

set(shadow_windows_resource_directory "${CMAKE_CURRENT_BINARY_DIR}/windows-resources")
set(shadow_windows_icon "${shadow_windows_resource_directory}/Shadow.ico")
file(MAKE_DIRECTORY "${shadow_windows_resource_directory}")
add_custom_command(
    OUTPUT "${shadow_windows_icon}"
    COMMAND "${CMAKE_COMMAND}" -E env
        --modify "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        -- "$<TARGET_FILE:shadow-windows-icon-generator>"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/app-icon-source.png" "${shadow_windows_icon}"
    DEPENDS shadow-windows-icon-generator "${CMAKE_CURRENT_SOURCE_DIR}/assets/app-icon-source.png"
    COMMENT "Generating Windows application icon sizes from the Shadow artwork"
    VERBATIM
)
add_custom_target(shadow-windows-icon DEPENDS "${shadow_windows_icon}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows_app_icon.rc.in"
    "${shadow_windows_resource_directory}/app-icon.rc"
    @ONLY
)
set_source_files_properties(
    "${shadow_windows_resource_directory}/app-icon.rc"
    PROPERTIES OBJECT_DEPENDS "${shadow_windows_icon}"
)
set(shadow_windows_resource_targets shadow-server-manager)
if(SHADOW_BUILD_DESKTOP)
    list(APPEND shadow_windows_resource_targets shadow-desktop)
endif()
foreach(app_target IN LISTS shadow_windows_resource_targets)
    add_dependencies(${app_target} shadow-windows-icon)
    target_sources(${app_target} PRIVATE "${shadow_windows_resource_directory}/app-icon.rc")
    qt_add_resources(${app_target} "${app_target}-branding"
        PREFIX "/branding" BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        FILES assets/app-icon-source.png)
endforeach()

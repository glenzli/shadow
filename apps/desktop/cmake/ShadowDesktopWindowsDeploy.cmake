# One deployment graph covers the photo app, server, and both isolated workers.
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
set(shadow_windows_app_targets shadow-server-manager)
set(shadow_windows_worker_targets)
if(SHADOW_BUILD_DESKTOP)
    list(APPEND shadow_windows_app_targets shadow-desktop)
    list(APPEND shadow_windows_worker_targets shadow-photo-composition-worker)
endif()
install(TARGETS ${shadow_windows_app_targets} ${shadow_windows_worker_targets}
    RUNTIME_DEPENDENCY_SET shadow_windows_runtime
    RUNTIME DESTINATION ".")
# Its imports are a subset of the applications' closure. Resolve those once to avoid treating
# identical app-local DLL copies in cpp/ and apps/ as conflicting runtime dependencies.
install(TARGETS shadow-image-decode-helper RUNTIME DESTINATION ".")
if(Lensfun_FOUND AND SHADOW_LENSFUN_DATABASE_PATH)
    install(DIRECTORY "${SHADOW_LENSFUN_DATABASE_PATH}/" DESTINATION Lensfun)
    foreach(app_target IN LISTS shadow_windows_app_targets)
        add_custom_command(TARGET ${app_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${SHADOW_LENSFUN_DATABASE_PATH}" "$<TARGET_FILE_DIR:${app_target}>/Lensfun")
    endforeach()
endif()
install(RUNTIME_DEPENDENCY_SET shadow_windows_runtime
    DIRECTORIES "$<TARGET_FILE_DIR:Qt6::Core>"
    PRE_EXCLUDE_REGEXES "^api-ms-.*" "^ext-ms-.*"
    POST_EXCLUDE_REGEXES ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\].*"
    RUNTIME DESTINATION ".")
install(CODE [[
    set(QT_DEPLOY_BIN_DIR ".")
    set(QT_DEPLOY_LIB_DIR ".")
    set(QT_DEPLOY_LIBEXEC_DIR ".")
    set(QT_DEPLOY_PLUGINS_DIR ".")
]])
foreach(app_target IN LISTS shadow_windows_app_targets)
    qt_generate_deploy_qml_app_script(TARGET ${app_target}
        OUTPUT_SCRIPT shadow_windows_deploy_script NO_TRANSLATIONS)
    install(SCRIPT "${shadow_windows_deploy_script}")
endforeach()
# Debug acceptance runs the deployed QML graph without opening test windows. windeployqt
# includes only qwindows by default, so keep its matching offscreen platform with test builds.
if(BUILD_TESTING AND TARGET Qt6::QOffscreenIntegrationPlugin)
    install(FILES "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>" DESTINATION platforms)
endif()
if(SHADOW_BUILD_DESKTOP)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/geonames/NOTICE.txt" DESTINATION GeoNames)
    if(SHADOW_GEONAMES_CITY_INDEX_PATH)
        install(FILES "${SHADOW_GEONAMES_CITY_INDEX_PATH}" DESTINATION GeoNames
            RENAME shadow-geonames-cities-v1.tsv)
    endif()
endif()

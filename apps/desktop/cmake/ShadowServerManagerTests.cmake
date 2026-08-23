if(BUILD_TESTING)
    add_test(
        NAME shadow-server-manager-theme-singleton
        COMMAND
            ${CMAKE_COMMAND}
            "-DSHADOW_SERVER_MANAGER_QMLDIR=$<TARGET_FILE_DIR:shadow-server-manager>/Shadow/ServerManager/qmldir"
            -P "${CMAKE_CURRENT_LIST_DIR}/VerifyServerManagerThemeSingleton.cmake"
    )
    add_test(
        NAME shadow-server-manager-qml-startup
        COMMAND $<TARGET_FILE:shadow-server-manager>
    )
    set_tests_properties(
        shadow-server-manager-qml-startup
        PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;SHADOW_SERVER_MANAGER_SMOKE_TEST=1;SHADOW_SERVER_MANAGER_DATA_ROOT=${CMAKE_CURRENT_BINARY_DIR}/server-manager-startup-smoke"
            TIMEOUT 15
    )
endif()

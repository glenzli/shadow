if(NOT EXISTS "${SHADOW_SERVER_MANAGER_QMLDIR}")
    message(FATAL_ERROR "Shadow Server QML module descriptor is missing: ${SHADOW_SERVER_MANAGER_QMLDIR}")
endif()

file(STRINGS "${SHADOW_SERVER_MANAGER_QMLDIR}" theme_singleton REGEX "^singleton Theme 1\\.0 ")
if(NOT theme_singleton)
    message(FATAL_ERROR
        "Shadow Server must register Theme.qml as a singleton; its visual tokens would otherwise resolve to QML defaults"
    )
endif()

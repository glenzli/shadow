# Application-shell and Qt/backend boundary contracts: startup, history,
# export, folder import, and production-linked backend projection.
    add_test(
        NAME shadow-desktop-qml-startup
        COMMAND $<TARGET_FILE:shadow-desktop>
    )

    add_executable(
        shadow-backend-history-projection-test
        tests/backend_history_projection_test.cpp
        ${SHADOW_DESKTOP_HISTORY_PROJECTION_SOURCES}
    )
    target_compile_features(
        shadow-backend-history-projection-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-backend-history-projection-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-backend-history-projection-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-backend-history-projection-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-backend-history-projection-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-backend-history-projection-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-backend-history-projection
        COMMAND shadow-backend-history-projection-test
    )

    add_executable(
        shadow-history-coordinator-test
        tests/history_coordinator_test.cpp
        src/history_coordinator.cpp
        src/history_coordinator.hpp
        src/history_model.cpp
        src/history_model.hpp
    )
    target_compile_features(
        shadow-history-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-history-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-history-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-history-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-history-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-history-coordinator
        COMMAND shadow-history-coordinator-test
    )

    add_executable(
        shadow-history-drawer-contract-test
        tests/history_drawer_contract_test.cpp
    )
    target_compile_features(
        shadow-history-drawer-contract-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-history-drawer-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-history-drawer-contract-test
        URI Shadow.HistoryDrawerContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/HistoryDrawer.qml
            qml/ShadowButton.qml
            qml/ShadowIcon.qml
            qml/ShadowIconButton.qml
            qml/ShadowTabButton.qml
            qml/Theme.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-history-drawer-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-history-drawer-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-history-drawer-contract
        COMMAND shadow-history-drawer-contract-test
    )
    set_tests_properties(
        shadow-desktop-history-drawer-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    set_tests_properties(
        shadow-desktop-qml-startup
        PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;SHADOW_DESKTOP_SMOKE_TEST=1;SHADOW_DESKTOP_DATA_ROOT=${CMAKE_CURRENT_BINARY_DIR}/qml-startup-smoke"
    )

    add_executable(
        shadow-backend-export-contract-test
        tests/backend_export_contract_test.cpp
    )
    target_compile_features(
        shadow-backend-export-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-backend-export-contract-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-backend-export-contract-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-backend-export-contract-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-backend-export-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-backend-export-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-backend-export-contract
        COMMAND shadow-backend-export-contract-test
    )

    add_executable(
        shadow-export-preset-store-test
        tests/export_preset_store_test.cpp
        src/backend/export_settings_codec.cpp
        src/backend/export_settings_codec.hpp
        src/export_preset_store.cpp
        src/export_preset_store.hpp
    )
    target_compile_features(
        shadow-export-preset-store-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-export-preset-store-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-export-preset-store-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-export-preset-store-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-export-preset-store-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-export-preset-store
        COMMAND shadow-export-preset-store-test
    )

    add_executable(
        shadow-folder-scan-backend-contract-test
        tests/folder_scan_backend_contract_test.cpp
        src/folder_scan_backend.cpp
        src/folder_scan_backend.hpp
    )
    target_compile_features(
        shadow-folder-scan-backend-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-folder-scan-backend-contract-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-folder-scan-backend-contract-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-folder-scan-backend-contract-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-folder-scan-backend-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-folder-scan-backend-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-folder-scan-backend-contract
        COMMAND shadow-folder-scan-backend-contract-test
    )

    add_executable(
        shadow-backend-photo-inspection-contract-test
        tests/backend_photo_inspection_contract_test.cpp
        ${SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES}
        src/desktop_backend.cpp
        src/desktop_backend.hpp
        src/folder_scan_backend.cpp
        src/folder_scan_backend.hpp
        src/photo_inspection_projection.cpp
        src/photo_inspection_projection.hpp
        src/preview_diagnostics.cpp
        src/preview_diagnostics.hpp
    )
    target_compile_features(
        shadow-backend-photo-inspection-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-backend-photo-inspection-contract-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-backend-photo-inspection-contract-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-backend-photo-inspection-contract-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-backend-photo-inspection-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-backend-photo-inspection-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-backend-photo-inspection-contract
        COMMAND shadow-backend-photo-inspection-contract-test
    )

    add_executable(
        shadow-review-source-health-backend-contract-test
        tests/review_source_health_backend_contract_test.cpp
        ${SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES}
        src/desktop_backend.cpp
        src/desktop_backend_library.cpp
        src/desktop_backend.hpp
        src/folder_scan_backend.cpp
        src/folder_scan_backend.hpp
        src/photo_inspection_projection.cpp
        src/photo_inspection_projection.hpp
        src/preview_diagnostics.cpp
        src/preview_diagnostics.hpp
    )
    target_compile_features(
        shadow-review-source-health-backend-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-source-health-backend-contract-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-review-source-health-backend-contract-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-review-source-health-backend-contract-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-review-source-health-backend-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-source-health-backend-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-source-health-backend-contract
        COMMAND shadow-review-source-health-backend-contract-test
    )

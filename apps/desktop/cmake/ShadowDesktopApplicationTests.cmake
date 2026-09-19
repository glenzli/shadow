# Application-shell and Qt/backend boundary contracts: startup, history,
# export, folder import, and production-linked backend projection.
    add_executable(shadow-export-workflow-contract-test
        tests/export_workflow_contract_test.cpp)
    target_compile_features(shadow-export-workflow-contract-test PRIVATE cxx_std_20)
    target_compile_definitions(shadow-export-workflow-contract-test PRIVATE
        SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(shadow-export-workflow-contract-test PRIVATE
        Qt6::Quick Qt6::Qml Qt6::Test)
    add_test(NAME shadow-desktop-export-workflow-contract
        COMMAND shadow-export-workflow-contract-test)
    set_tests_properties(shadow-desktop-export-workflow-contract PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
        LABELS "desktop;qml;export" TIMEOUT 30)

    add_test(
        NAME shadow-desktop-qml-startup
        COMMAND $<TARGET_FILE:shadow-desktop>
    )
    add_test(
        NAME shadow-desktop-map-workspace-startup
        COMMAND $<TARGET_FILE:shadow-desktop>
    )

    add_executable(
        shadow-people-analysis-controller-test
        tests/people_analysis_controller_test.cpp
        src/ai_preferences.cpp
        src/ai_preferences.hpp
        src/people_analysis_controller.cpp
        src/people_analysis_controller.hpp
    )
    target_compile_features(shadow-people-analysis-controller-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-people-analysis-controller-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-people-analysis-controller-test
        PRIVATE Qt6::Concurrent Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-people-analysis-controller-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-people-analysis-controller-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-people-analysis-controller
        COMMAND shadow-people-analysis-controller-test
    )

    add_executable(
        shadow-semantic-search-controller-test
        tests/semantic_search_controller_test.cpp
        src/semantic_search_controller.cpp
        src/semantic_search_controller.hpp
    )
    target_compile_features(shadow-semantic-search-controller-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-semantic-search-controller-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-semantic-search-controller-test
        PRIVATE Qt6::Concurrent Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-semantic-search-controller-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-semantic-search-controller-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-semantic-search-controller
        COMMAND shadow-semantic-search-controller-test
    )

    add_executable(
        shadow-smart-category-controller-test
        tests/smart_category_controller_test.cpp
        src/smart_category_controller.cpp
        src/smart_category_controller.hpp
    )
    target_compile_features(shadow-smart-category-controller-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-smart-category-controller-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-smart-category-controller-test
        PRIVATE Qt6::Concurrent Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-smart-category-controller-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-smart-category-controller-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-smart-category-controller
        COMMAND shadow-smart-category-controller-test
    )

    add_executable(
        shadow-review-smart-category-feedback-popup-test
        tests/review_smart_category_feedback_popup_test.cpp
    )
    target_compile_features(
        shadow-review-smart-category-feedback-popup-test PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-review-smart-category-feedback-popup-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test
    )
    qt_add_qml_module(
        shadow-review-smart-category-feedback-popup-test
        URI Shadow.SmartCategoryFeedbackContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/ReviewSmartCategoryFeedbackPopup.qml
            qml/ShadowButton.qml
            qml/ShadowCheckBox.qml
            qml/ShadowIcon.qml
            qml/Theme.qml
    )
    qt_add_resources(
        shadow-review-smart-category-feedback-popup-test
        shadow-review-smart-category-feedback-popup-test-icons
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES icons/check.svg
    )
    if(MSVC)
        target_compile_options(
            shadow-review-smart-category-feedback-popup-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-smart-category-feedback-popup-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-smart-category-feedback-popup
        COMMAND shadow-review-smart-category-feedback-popup-test
    )
    set_tests_properties(
        shadow-desktop-review-smart-category-feedback-popup
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-image-understanding-controller-test
        tests/image_understanding_controller_test.cpp
        src/image_understanding_controller.cpp
        src/image_understanding_controller.hpp
        src/ai_preferences.cpp
        src/ai_preferences.hpp
    )
    target_compile_features(shadow-image-understanding-controller-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-image-understanding-controller-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-image-understanding-controller-test
        PRIVATE Qt6::Concurrent Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-image-understanding-controller-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-image-understanding-controller-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-image-understanding-controller
        COMMAND shadow-image-understanding-controller-test
    )

    add_executable(
        shadow-people-workspace-contract-test
        tests/people_workspace_contract_test.cpp
    )
    target_compile_features(shadow-people-workspace-contract-test PRIVATE cxx_std_20)
    target_link_libraries(
        shadow-people-workspace-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Svg
    )
    qt_add_qml_module(
        shadow-people-workspace-contract-test
        URI Shadow.PeopleWorkspaceContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PeopleWorkspace.qml
            qml/ShadowButton.qml
            qml/ShadowIcon.qml
            qml/Theme.qml
    )
    qt_add_resources(
        shadow-people-workspace-contract-test people-workspace-contract-icons
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES
            icons/people.svg
            icons/storage.svg
    )
    if(MSVC)
        target_compile_options(
            shadow-people-workspace-contract-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-people-workspace-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-people-workspace-contract
        COMMAND shadow-people-workspace-contract-test
    )
    set_tests_properties(
        shadow-desktop-people-workspace-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
    add_test(
        NAME shadow-server-manager-qml-startup
        COMMAND $<TARGET_FILE:shadow-server-manager>
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
    set_tests_properties(
        shadow-desktop-map-workspace-startup
        PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;SHADOW_DESKTOP_SMOKE_TEST=1;SHADOW_DESKTOP_MAP_WORKSPACE_SMOKE=1;SHADOW_DESKTOP_DATA_ROOT=${CMAKE_CURRENT_BINARY_DIR}/map-workspace-startup-smoke"
    )
    set_tests_properties(
        shadow-server-manager-qml-startup
        PROPERTIES
            ENVIRONMENT
                "QT_QPA_PLATFORM=offscreen;SHADOW_SERVER_MANAGER_SMOKE_TEST=1;SHADOW_SERVER_MANAGER_DATA_ROOT=${CMAKE_CURRENT_BINARY_DIR}/server-manager-startup-smoke"
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
        shadow-export-raster-encoder-contract-test
        tests/export_raster_encoder_contract_test.cpp
        src/backend/export_raster_encoder.cpp
        src/backend/export_raster_encoder.hpp
    )
    target_compile_features(
        shadow-export-raster-encoder-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-export-raster-encoder-contract-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-export-raster-encoder-contract-test
        PRIVATE Qt6::Core Qt6::Gui TIFF::TIFF
    )
    if(MSVC)
        target_compile_options(
            shadow-export-raster-encoder-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-export-raster-encoder-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-export-raster-encoder-contract
        COMMAND shadow-export-raster-encoder-contract-test
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
        shadow-export-watermark-store-test
        tests/export_watermark_store_test.cpp
        src/export_watermark_store.cpp
        src/export_watermark_store.hpp
    )
    target_compile_features(
        shadow-export-watermark-store-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-export-watermark-store-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-export-watermark-store-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-export-watermark-store-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-export-watermark-store-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-export-watermark-store
        COMMAND shadow-export-watermark-store-test
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

    add_executable(shadow-catalog-startup-recovery-test
        tests/catalog_startup_recovery_test.cpp src/catalog_startup_recovery.cpp src/catalog_startup_recovery.hpp)
    target_compile_features(shadow-catalog-startup-recovery-test PRIVATE cxx_std_20)
    target_include_directories(shadow-catalog-startup-recovery-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(shadow-catalog-startup-recovery-test PRIVATE Qt6::Core Qt6::Widgets)
    add_test(NAME shadow-desktop-catalog-startup-recovery COMMAND shadow-catalog-startup-recovery-test)

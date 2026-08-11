# Device-local preferences, profile, settings presentation, and map/location
# service contracts.
    add_executable(
        shadow-ui-preferences-test
        tests/ui_preferences_test.cpp
        src/ui_preferences.cpp
        src/ui_preferences.hpp
    )
    target_compile_features(shadow-ui-preferences-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-ui-preferences-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-ui-preferences-test
        PRIVATE Qt6::Gui Qt6::Qml
    )
    if(MSVC)
        target_compile_options(shadow-ui-preferences-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-ui-preferences-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-ui-preferences COMMAND shadow-ui-preferences-test)

    add_executable(
        shadow-ai-preferences-test
        tests/ai_preferences_test.cpp
        src/ai_preferences.cpp
        src/ai_preferences.hpp
    )
    target_compile_features(shadow-ai-preferences-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-ai-preferences-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-ai-preferences-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(shadow-ai-preferences-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-ai-preferences-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-ai-preferences COMMAND shadow-ai-preferences-test)

    add_executable(
        shadow-personal-profile-test
        tests/personal_profile_test.cpp
        src/personal_profile.cpp
        src/personal_profile.hpp
    )
    target_compile_features(shadow-personal-profile-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-personal-profile-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-personal-profile-test PRIVATE Qt6::Gui)
    if(MSVC)
        target_compile_options(shadow-personal-profile-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-personal-profile-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-personal-profile COMMAND shadow-personal-profile-test)

    add_executable(
        shadow-cache-preferences-test
        tests/cache_preferences_test.cpp
        src/cache_preferences.cpp
        src/cache_preferences.hpp
    )
    target_compile_features(shadow-cache-preferences-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-cache-preferences-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-cache-preferences-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(shadow-cache-preferences-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-cache-preferences-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-cache-preferences COMMAND shadow-cache-preferences-test)

    add_executable(
        shadow-library-server-controller-test
        tests/library_server_controller_test.cpp
        src/library_server_controller.cpp
        src/library_server_controller.hpp
        src/secure_secret_store.hpp
    )
    target_compile_features(shadow-library-server-controller-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-library-server-controller-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-library-server-controller-test
        PRIVATE Qt6::Concurrent Qt6::Core Qt6::Gui Qt6::Network
    )
    if(MSVC)
        target_compile_options(
            shadow-library-server-controller-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-library-server-controller-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-library-server-controller
        COMMAND shadow-library-server-controller-test
    )

    add_executable(
        shadow-map-provider-preferences-test
        tests/map_provider_preferences_test.cpp
        src/map_provider_preferences.cpp
        src/map_provider_preferences.hpp
        src/secure_secret_store.hpp
    )
    target_compile_features(
        shadow-map-provider-preferences-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-map-provider-preferences-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-map-provider-preferences-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-map-provider-preferences-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-map-provider-preferences-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-map-provider-preferences
        COMMAND shadow-map-provider-preferences-test
    )

    add_executable(
        shadow-amap-coordinate-transform-test
        tests/amap_coordinate_transform_test.cpp
        src/amap_coordinate_transform.cpp
        src/amap_coordinate_transform.hpp
    )
    target_compile_features(shadow-amap-coordinate-transform-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-amap-coordinate-transform-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-amap-coordinate-transform-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(shadow-amap-coordinate-transform-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-amap-coordinate-transform-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-amap-coordinate-transform
        COMMAND shadow-amap-coordinate-transform-test
    )

    add_executable(
        shadow-amap-web-service-protocol-test
        tests/amap_web_service_protocol_test.cpp
        src/amap_web_service_protocol.cpp
        src/amap_web_service_protocol.hpp
    )
    target_compile_features(shadow-amap-web-service-protocol-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-amap-web-service-protocol-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-amap-web-service-protocol-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(shadow-amap-web-service-protocol-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-amap-web-service-protocol-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-amap-web-service-protocol
        COMMAND shadow-amap-web-service-protocol-test
    )

    add_executable(
        shadow-amap-library-reverse-geocoder-test
        tests/amap_library_reverse_geocoder_test.cpp
        src/amap_coordinate_transform.cpp
        src/amap_coordinate_transform.hpp
        src/amap_web_service_protocol.cpp
        src/amap_web_service_protocol.hpp
        src/amap_library_reverse_geocoder.cpp
        src/amap_library_reverse_geocoder.hpp
        src/library_reverse_geocoder.hpp
        src/map_provider_preferences.cpp
        src/map_provider_preferences.hpp
        src/secure_secret_store.hpp
    )
    target_compile_features(shadow-amap-library-reverse-geocoder-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-amap-library-reverse-geocoder-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-amap-library-reverse-geocoder-test
        PRIVATE Qt6::Core Qt6::Network
    )
    if(MSVC)
        target_compile_options(
            shadow-amap-library-reverse-geocoder-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-amap-library-reverse-geocoder-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-amap-library-reverse-geocoder
        COMMAND shadow-amap-library-reverse-geocoder-test
    )

    add_executable(
        shadow-amap-place-search-service-test
        tests/amap_place_search_service_test.cpp
        src/amap_coordinate_transform.cpp
        src/amap_coordinate_transform.hpp
        src/amap_web_service_protocol.cpp
        src/amap_web_service_protocol.hpp
        src/amap_place_search_service.cpp
        src/amap_place_search_service.hpp
        src/map_provider_preferences.cpp
        src/map_provider_preferences.hpp
        src/secure_secret_store.hpp
    )
    target_compile_features(shadow-amap-place-search-service-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-amap-place-search-service-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-amap-place-search-service-test
        PRIVATE Qt6::Core Qt6::Network
    )
    if(MSVC)
        target_compile_options(
            shadow-amap-place-search-service-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-amap-place-search-service-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-amap-place-search-service
        COMMAND shadow-amap-place-search-service-test
    )

    add_executable(
        shadow-library-web-map-controller-test
        tests/library_web_map_controller_test.cpp
        src/amap_coordinate_transform.cpp
        src/amap_coordinate_transform.hpp
        src/map/library_web_map_controller.cpp
        src/map/library_web_map_controller.hpp
        src/map_provider_preferences.cpp
        src/map_provider_preferences.hpp
        src/secure_secret_store.hpp
    )
    target_compile_features(shadow-library-web-map-controller-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-library-web-map-controller-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-library-web-map-controller-test
        PRIVATE Qt6::Core Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-library-web-map-controller-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-library-web-map-controller-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-library-web-map-controller
        COMMAND shadow-library-web-map-controller-test
    )

    add_executable(
        shadow-google-library-reverse-geocoder-test
        tests/google_library_reverse_geocoder_test.cpp
        src/google_library_reverse_geocoder.cpp
        src/google_library_reverse_geocoder.hpp
        src/library_reverse_geocoder.hpp
        src/map_provider_preferences.cpp
        src/map_provider_preferences.hpp
        src/secure_secret_store.hpp
    )
    target_compile_features(
        shadow-google-library-reverse-geocoder-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-google-library-reverse-geocoder-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-google-library-reverse-geocoder-test
        PRIVATE Qt6::Core Qt6::Network
    )
    if(MSVC)
        target_compile_options(
            shadow-google-library-reverse-geocoder-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-google-library-reverse-geocoder-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-google-library-reverse-geocoder
        COMMAND shadow-google-library-reverse-geocoder-test
    )

    add_executable(
        shadow-geonames-city-index-test
        tests/geonames_city_index_test.cpp
        src/geonames_city_index.cpp
        src/geonames_city_index.hpp
    )
    target_compile_features(shadow-geonames-city-index-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-geonames-city-index-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-geonames-city-index-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(shadow-geonames-city-index-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-geonames-city-index-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-geonames-city-index
        COMMAND shadow-geonames-city-index-test
    )

    add_executable(
        shadow-personal-location-search-test
        tests/personal_location_search_test.cpp
        src/personal_location_search.cpp
        src/personal_location_search.hpp
        src/geonames_city_index.cpp
        src/geonames_city_index.hpp
    )
    target_compile_features(shadow-personal-location-search-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-personal-location-search-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-personal-location-search-test
        PRIVATE Qt6::Core Qt6::Concurrent
    )
    if(MSVC)
        target_compile_options(shadow-personal-location-search-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-personal-location-search-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-personal-location-search
        COMMAND shadow-personal-location-search-test
    )

    add_executable(
        shadow-geonames-library-reverse-geocoder-test
        tests/geonames_library_reverse_geocoder_test.cpp
        src/geonames_city_index.cpp
        src/geonames_city_index.hpp
        src/geonames_library_reverse_geocoder.cpp
        src/geonames_library_reverse_geocoder.hpp
        src/library_reverse_geocoder.hpp
    )
    target_compile_features(
        shadow-geonames-library-reverse-geocoder-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-geonames-library-reverse-geocoder-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-geonames-library-reverse-geocoder-test
        PRIVATE Qt6::Core Qt6::Concurrent
    )
    if(MSVC)
        target_compile_options(
            shadow-geonames-library-reverse-geocoder-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-geonames-library-reverse-geocoder-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-geonames-library-reverse-geocoder
        COMMAND shadow-geonames-library-reverse-geocoder-test
    )

    add_executable(
        shadow-library-reverse-geocoder-router-test
        tests/library_reverse_geocoder_router_test.cpp
        src/library_reverse_geocoder_router.cpp
        src/library_reverse_geocoder_router.hpp
        src/library_reverse_geocoder.hpp
    )
    target_compile_features(shadow-library-reverse-geocoder-router-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-library-reverse-geocoder-router-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-library-reverse-geocoder-router-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(
            shadow-library-reverse-geocoder-router-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-library-reverse-geocoder-router-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-library-reverse-geocoder-router
        COMMAND shadow-library-reverse-geocoder-router-test
    )

    add_executable(
        shadow-secure-secret-store-test
        tests/secure_secret_store_test.cpp
        src/secure_secret_store.cpp
        src/secure_secret_store.hpp
    )
    target_compile_features(
        shadow-secure-secret-store-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-secure-secret-store-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-secure-secret-store-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-secure-secret-store-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-secure-secret-store-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-secure-secret-store
        COMMAND shadow-secure-secret-store-test
    )

    add_executable(
        shadow-map-provider-settings-dialog-test
        tests/map_provider_settings_dialog_test.cpp
    )
    target_compile_features(
        shadow-map-provider-settings-dialog-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-map-provider-settings-dialog-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-map-provider-settings-dialog-test
        URI Shadow.MapProviderSettingsContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/MapProviderSettingsDialog.qml
            qml/MapProviderSettingsPane.qml
            qml/ShadowButton.qml
            qml/ShadowSwitch.qml
            qml/Theme.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-map-provider-settings-dialog-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-map-provider-settings-dialog-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-map-provider-settings-dialog
        COMMAND shadow-map-provider-settings-dialog-test
    )
    set_tests_properties(
        shadow-desktop-map-provider-settings-dialog
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-application-settings-dialog-test
        tests/application_settings_dialog_test.cpp
    )
    target_compile_features(
        shadow-application-settings-dialog-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-application-settings-dialog-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-application-settings-dialog-test
        URI Shadow.ApplicationSettingsContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/ApplicationSettingsDialog.qml
            qml/SettingsGeneralPane.qml
            qml/SettingsLibraryPane.qml
            qml/SettingsAiPane.qml
            qml/SettingsStoragePane.qml
            qml/MapProviderSettingsPane.qml
            qml/ShadowButton.qml
            qml/ShadowCheckBox.qml
            qml/ShadowIcon.qml
            qml/ShadowInlineSlider.qml
            qml/ShadowSlider.qml
            qml/ShadowSwitch.qml
            qml/Theme.qml
    )
    qt_add_resources(
        shadow-application-settings-dialog-test
        shadow-application-settings-dialog-test-icons
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES
            icons/add-folder.svg
            icons/check.svg
            icons/map.svg
            icons/mask.svg
            icons/review-grid.svg
            icons/settings.svg
    )
    if(MSVC)
        target_compile_options(
            shadow-application-settings-dialog-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-application-settings-dialog-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-application-settings-dialog
        COMMAND shadow-application-settings-dialog-test
    )
    set_tests_properties(
        shadow-desktop-application-settings-dialog
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-metadata-field-selector-row-test
        tests/metadata_field_selector_row_test.cpp
    )
    target_compile_features(
        shadow-metadata-field-selector-row-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-metadata-field-selector-row-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-metadata-field-selector-row-test
        URI Shadow.MetadataFieldSelectorContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/MetadataFieldSelectorRow.qml
            qml/ShadowCheckBox.qml
            qml/ShadowIcon.qml
            qml/Theme.qml
    )
    qt_add_resources(
        shadow-metadata-field-selector-row-test
        shadow-metadata-field-selector-row-test-icons
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES icons/check.svg
    )
    if(MSVC)
        target_compile_options(
            shadow-metadata-field-selector-row-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-metadata-field-selector-row-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-metadata-field-selector-row
        COMMAND shadow-metadata-field-selector-row-test
    )
    set_tests_properties(
        shadow-desktop-metadata-field-selector-row
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-library-map-provider-overlay-test
        tests/library_map_provider_overlay_test.cpp
    )
    target_compile_features(
        shadow-library-map-provider-overlay-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-library-map-provider-overlay-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-library-map-provider-overlay-test
        URI Shadow.LibraryMapProviderOverlayContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/LibraryMapProviderOverlay.qml
            qml/ShadowButton.qml
            qml/Theme.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-library-map-provider-overlay-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-library-map-provider-overlay-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-library-map-provider-overlay
        COMMAND shadow-library-map-provider-overlay-test
    )
    set_tests_properties(
        shadow-desktop-library-map-provider-overlay
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-library-map-place-search-test
        tests/library_map_place_search_test.cpp
    )
    target_compile_features(shadow-library-map-place-search-test PRIVATE cxx_std_20)
    target_link_libraries(
        shadow-library-map-place-search-test
        PRIVATE
            Qt6::Gui
            Qt6::Qml
            Qt6::Quick
            Qt6::QuickControls2
            Qt6::Test
    )
    qt_add_qml_module(
        shadow-library-map-place-search-test
        URI Shadow.LibraryMapPlaceSearchContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/LibraryMapPlaceSearch.qml
            qml/ShadowIconButton.qml
            qml/ShadowIcon.qml
            qml/Theme.qml
    )
    qt_add_resources(
        shadow-library-map-place-search-test
        shadow-library-map-place-search-test-icons
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES icons/close.svg
    )
    if(MSVC)
        target_compile_options(
            shadow-library-map-place-search-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-library-map-place-search-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-library-map-place-search
        COMMAND shadow-library-map-place-search-test
    )
    set_tests_properties(
        shadow-desktop-library-map-place-search
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-library-remote-connections-pane-test
        tests/library_remote_connections_pane_test.cpp
    )
    target_compile_features(
        shadow-library-remote-connections-pane-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-library-remote-connections-pane-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-library-remote-connections-pane-test
        URI Shadow.LibraryRemoteConnectionsContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/LibraryRemoteConnectionsPane.qml
            qml/ShadowButton.qml
            qml/ShadowIcon.qml
            qml/Theme.qml
    )
    qt_add_resources(
        shadow-library-remote-connections-pane-test
        shadow-library-remote-connections-pane-icons
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES
            icons/add-folder.svg
            icons/shared-link.svg
    )
    if(MSVC)
        target_compile_options(
            shadow-library-remote-connections-pane-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-library-remote-connections-pane-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-library-remote-connections-pane
        COMMAND shadow-library-remote-connections-pane-test
    )
    set_tests_properties(
        shadow-desktop-library-remote-connections-pane
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

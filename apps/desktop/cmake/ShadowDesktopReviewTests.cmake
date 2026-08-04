# Review models, evidence/decision workflows, Library coordinators, and
# packaged Review collection/profile contracts.
    add_executable(
        shadow-review-visual-request-contract-test
        tests/review_visual_request_contract_test.cpp
        src/review_visual_request.cpp
    )
    target_compile_features(
        shadow-review-visual-request-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-visual-request-contract-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-visual-request-contract-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-review-visual-request-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-visual-request-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-visual-request-contract
        COMMAND shadow-review-visual-request-contract-test
    )

    add_executable(
        shadow-review-photo-inspection-session-test
        tests/review_photo_inspection_session_test.cpp
        src/review_photo_inspection_session.hpp
    )
    target_compile_features(
        shadow-review-photo-inspection-session-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-photo-inspection-session-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-photo-inspection-session-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-review-photo-inspection-session-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-photo-inspection-session-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-photo-inspection-session
        COMMAND shadow-review-photo-inspection-session-test
    )

    add_executable(
        shadow-review-photo-inspection-coordinator-test
        tests/review_photo_inspection_coordinator_test.cpp
        ${SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES}
        src/review_photo_inspection_coordinator.cpp
        src/review_photo_inspection_coordinator.hpp
        src/review_photo_inspection_session.hpp
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
        shadow-review-photo-inspection-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-photo-inspection-coordinator-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-review-photo-inspection-coordinator-test
        PRIVATE
            shadow-desktop-export-backend
            Qt6::Concurrent
    )
    add_dependencies(
        shadow-review-photo-inspection-coordinator-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-review-photo-inspection-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-photo-inspection-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-photo-inspection-coordinator
        COMMAND shadow-review-photo-inspection-coordinator-test
    )

    add_executable(
        shadow-review-model-test
        tests/review_model_test.cpp
        tests/review_model/review_model_fixture.hpp
        tests/review_model/role_projection_contract.cpp
        tests/review_model/state_projection_contract.cpp
        tests/review_model/visual_generation_contract.cpp
        tests/review_model/item_projection_contract.cpp
        tests/review_model/snapshot_membership_contract.cpp
        tests/review_model/remote_library_projection_contract.cpp
        src/review_model.cpp
        src/review_visual_request.cpp
    )
    target_compile_features(shadow-review-model-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-review-model-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-review-model-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(shadow-review-model-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-review-model-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-review-model COMMAND shadow-review-model-test)

    add_executable(
        shadow-review-remote-library-coordinator-test
        tests/review_remote_library_coordinator_test.cpp
        src/remote_library_connection_store.cpp
        src/remote_library_connection_store.hpp
        src/review_remote_library_coordinator.cpp
        src/review_remote_library_coordinator.hpp
        src/review_model.cpp
        src/review_model.hpp
        src/review_visual_request.cpp
        src/review_visual_request.hpp
        src/secure_secret_store.cpp
        src/secure_secret_store.hpp
    )
    target_compile_features(
        shadow-review-remote-library-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-remote-library-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-remote-library-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-review-remote-library-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-remote-library-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-remote-library-coordinator
        COMMAND shadow-review-remote-library-coordinator-test
    )

    add_executable(
        shadow-review-filter-model-test
        tests/review_filter_model_test.cpp
        src/review_filter_model.cpp
        src/review_filter_model.hpp
        src/review_model.cpp
        src/review_model.hpp
        src/review_visual_request.cpp
        src/review_visual_request.hpp
    )
    target_compile_features(
        shadow-review-filter-model-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-filter-model-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-review-filter-model-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(
            shadow-review-filter-model-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-filter-model-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-filter-model
        COMMAND shadow-review-filter-model-test
    )

    add_executable(
        shadow-justified-review-layout-model-test
        tests/justified_review_layout_model_test.cpp
        src/justified_review_layout_model.cpp
        src/justified_review_layout_model.hpp
        src/review_model.cpp
        src/review_model.hpp
        src/review_visual_request.cpp
        src/review_visual_request.hpp
    )
    target_compile_features(
        shadow-justified-review-layout-model-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-justified-review-layout-model-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-justified-review-layout-model-test
        PRIVATE Qt6::Core
    )
    if(MSVC)
        target_compile_options(
            shadow-justified-review-layout-model-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-justified-review-layout-model-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-justified-review-layout-model
        COMMAND shadow-justified-review-layout-model-test
    )

    add_executable(
        shadow-review-evidence-session-test
        tests/review_evidence_session_test.cpp
        src/review_evidence_session.hpp
    )
    target_compile_features(shadow-review-evidence-session-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-review-evidence-session-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-review-evidence-session-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(
            shadow-review-evidence-session-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-evidence-session-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-evidence-session
        COMMAND shadow-review-evidence-session-test
    )

    add_executable(
        shadow-review-comparison-coordinator-test
        tests/review_comparison_coordinator_test.cpp
        tests/review_comparison_coordinator/comparison_backend_fixture.hpp
        tests/review_comparison_coordinator/evidence_lifecycle_contract.cpp
        tests/review_comparison_coordinator/failure_lifetime_contract.cpp
        tests/review_comparison_coordinator/presentation_contract.cpp
        tests/review_comparison_coordinator/receipt_validation_contract.cpp
        src/review_comparison_coordinator.cpp
        src/review_comparison_coordinator.hpp
        src/review_evidence_session.hpp
    )
    target_compile_features(
        shadow-review-comparison-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-comparison-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-comparison-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-comparison-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-comparison-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-comparison-coordinator
        COMMAND shadow-review-comparison-coordinator-test
    )

    add_executable(
        shadow-review-decision-coordinator-test
        tests/review_decision_coordinator_test.cpp
        tests/review_decision_coordinator/decision_coordinator_fixture.hpp
        tests/review_decision_coordinator/admission_projection_contract.cpp
        tests/review_decision_coordinator/failure_undo_contract.cpp
        tests/review_decision_coordinator/lifetime_contract.cpp
        src/review_decision_coordinator.cpp
        src/review_decision_coordinator.hpp
        src/review_decision_session.hpp
    )
    target_compile_features(
        shadow-review-decision-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-decision-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-decision-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-decision-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-decision-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-decision-coordinator
        COMMAND shadow-review-decision-coordinator-test
    )

    add_executable(
        shadow-review-source-health-coordinator-test
        tests/review_source_health_coordinator_test.cpp
        tests/review_source_health_coordinator/source_health_fixture.hpp
        tests/review_source_health_coordinator/source_health_refresh_contract.cpp
        tests/review_source_health_coordinator/missing_location_review_contract.cpp
        tests/review_source_health_coordinator/relink_lifetime_contract.cpp
        src/review_source_health_coordinator.cpp
        src/review_source_health_coordinator.hpp
    )
    target_compile_features(
        shadow-review-source-health-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-source-health-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-source-health-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-source-health-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-source-health-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-source-health-coordinator
        COMMAND shadow-review-source-health-coordinator-test
    )

    add_executable(
        shadow-review-library-album-coordinator-test
        tests/review_library_album_coordinator_test.cpp
        tests/review_library_album_coordinator/library_album_fixture.hpp
        tests/review_library_album_coordinator/album_lifecycle_contract.cpp
        tests/review_library_album_coordinator/refresh_failure_lifetime_contract.cpp
        src/review_library_album_coordinator.cpp
        src/review_library_album_coordinator.hpp
    )
    target_compile_features(
        shadow-review-library-album-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-album-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-album-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-album-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-album-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-album-coordinator
        COMMAND shadow-review-library-album-coordinator-test
    )

    add_executable(
        shadow-review-library-keyword-coordinator-test
        tests/review_library_keyword_coordinator_test.cpp
        tests/review_library_keyword_coordinator/library_keyword_fixture.hpp
        tests/review_library_keyword_coordinator/keyword_lifecycle_contract.cpp
        tests/review_library_keyword_coordinator/failure_lifetime_contract.cpp
        src/review_library_keyword_coordinator.cpp
        src/review_library_keyword_coordinator.hpp
    )
    target_compile_features(
        shadow-review-library-keyword-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-keyword-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-keyword-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-keyword-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-keyword-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-keyword-coordinator
        COMMAND shadow-review-library-keyword-coordinator-test
    )

    add_executable(
        shadow-review-library-facet-coordinator-test
        tests/review_library_facet_coordinator_test.cpp
        tests/review_library_facet_coordinator/library_facet_fixture.hpp
        tests/review_library_facet_coordinator/facet_projection_contract.cpp
        tests/review_library_facet_coordinator/coalescing_failure_lifetime_contract.cpp
        src/review_library_facet_coordinator.cpp
        src/review_library_facet_coordinator.hpp
    )
    target_compile_features(
        shadow-review-library-facet-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-facet-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-facet-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-facet-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-facet-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-facet-coordinator
        COMMAND shadow-review-library-facet-coordinator-test
    )

    add_executable(
        shadow-review-travel-collection-coordinator-test
        tests/review_travel_collection_coordinator_test.cpp
        src/review_travel_collection_coordinator.cpp
        src/review_travel_collection_coordinator.hpp
    )
    target_compile_features(
        shadow-review-travel-collection-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-travel-collection-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-travel-collection-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-travel-collection-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-travel-collection-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-travel-collection-coordinator
        COMMAND shadow-review-travel-collection-coordinator-test
    )

    add_executable(
        shadow-review-library-place-resolution-coordinator-test
        tests/review_library_place_resolution_coordinator_test.cpp
        src/review_library_place_resolution_coordinator.cpp
        src/review_library_place_resolution_coordinator.hpp
        src/library_reverse_geocoder.hpp
    )
    target_compile_features(
        shadow-review-library-place-resolution-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-place-resolution-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-place-resolution-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-place-resolution-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-place-resolution-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-place-resolution-coordinator
        COMMAND shadow-review-library-place-resolution-coordinator-test
    )

    add_executable(
        shadow-review-library-map-coordinator-test
        tests/review_library_map_coordinator_test.cpp
        src/review_library_map_coordinator.cpp
        src/review_library_map_coordinator.hpp
    )
    target_compile_features(
        shadow-review-library-map-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-map-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-map-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-map-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-map-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-map-coordinator
        COMMAND shadow-review-library-map-coordinator-test
    )

    add_executable(
        shadow-library-map-location-placement-test
        tests/library_map_location_placement_test.cpp
    )
    target_compile_features(
        shadow-library-map-location-placement-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-library-map-location-placement-test
        PRIVATE Qt6::Core Qt6::Qml
    )
    qt_add_qml_module(
        shadow-library-map-location-placement-test
        URI Shadow.LibraryMapLocationPlacementContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/LibraryMapLocationPlacementState.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-library-map-location-placement-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-library-map-location-placement-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-library-map-location-placement
        COMMAND shadow-library-map-location-placement-test
    )

    add_executable(
        shadow-review-library-organization-coordinator-test
        tests/review_library_organization_coordinator_test.cpp
        tests/review_library_organization_coordinator/library_organization_fixture.hpp
        tests/review_library_organization_coordinator/mutation_projection_contract.cpp
        tests/review_library_organization_coordinator/failure_lifetime_contract.cpp
        src/review_library_organization_coordinator.cpp
        src/review_library_organization_coordinator.hpp
    )
    target_compile_features(
        shadow-review-library-organization-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-organization-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-organization-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-organization-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-organization-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-organization-coordinator
        COMMAND shadow-review-library-organization-coordinator-test
    )

    add_executable(
        shadow-review-import-coordinator-test
        tests/review_import_coordinator_test.cpp
        tests/review_import_coordinator/import_coordinator_fixture.hpp
        tests/review_import_coordinator/import_progress_contract.cpp
        tests/review_import_coordinator/cancellation_failure_lifetime_contract.cpp
        src/review_import_coordinator.cpp
        src/review_import_coordinator.hpp
    )
    target_compile_features(
        shadow-review-import-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-import-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-import-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-import-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-import-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-import-coordinator
        COMMAND shadow-review-import-coordinator-test
    )

    add_executable(
        shadow-review-library-query-coordinator-test
        tests/review_library_query_coordinator_test.cpp
        tests/review_library_query_coordinator/library_query_fixture.hpp
        tests/review_library_query_coordinator/projection_pagination_contract.cpp
        tests/review_library_query_coordinator/coalescing_failure_lifetime_contract.cpp
        src/review_library_query_coordinator.cpp
        src/review_library_query_coordinator.hpp
        src/review_decision_coordinator.cpp
        src/review_decision_coordinator.hpp
        src/review_model.cpp
        src/review_model.hpp
        src/review_visual_request.cpp
        src/review_visual_request.hpp
    )
    target_compile_features(
        shadow-review-library-query-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-library-query-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-library-query-coordinator-test
        PRIVATE Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-library-query-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-library-query-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-library-query-coordinator
        COMMAND shadow-review-library-query-coordinator-test
    )

    add_executable(
        shadow-review-shared-grade-coordinator-test
        tests/review_shared_grade_coordinator_test.cpp
        tests/review_shared_grade_coordinator/shared_grade_fixture.hpp
        tests/review_shared_grade_coordinator/projection_apply_contract.cpp
        tests/review_shared_grade_coordinator/failure_admission_contract.cpp
        src/review_shared_grade_coordinator.cpp
        src/review_shared_grade_coordinator.hpp
    )
    target_compile_features(
        shadow-review-shared-grade-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-review-shared-grade-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-review-shared-grade-coordinator-test
        PRIVATE Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-review-shared-grade-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-shared-grade-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-shared-grade-coordinator
        COMMAND shadow-review-shared-grade-coordinator-test
    )

    add_executable(
        shadow-review-decision-session-test
        tests/review_decision_session_test.cpp
        src/review_decision_session.hpp
    )
    target_compile_features(shadow-review-decision-session-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-review-decision-session-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-review-decision-session-test PRIVATE Qt6::Gui)
    if(MSVC)
        target_compile_options(
            shadow-review-decision-session-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-decision-session-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-decision-session
        COMMAND shadow-review-decision-session-test
    )

    add_executable(
        shadow-personal-profile-dialog-test
        tests/personal_profile_dialog_test.cpp
        src/personal_profile.cpp
        src/personal_profile.hpp
    )
    target_compile_features(shadow-personal-profile-dialog-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-personal-profile-dialog-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-personal-profile-dialog-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-personal-profile-dialog-test
        URI Shadow.PersonalProfileContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PersonalProfileDialog.qml
            qml/PersonalLocationSearchField.qml
            qml/PersonalLivingPlacesEditor.qml
            qml/ShadowButton.qml
            qml/ShadowIcon.qml
            qml/ShadowRoundedImage.qml
            qml/Theme.qml
    )
    qt_add_resources(
        shadow-personal-profile-dialog-test
        shadow-personal-profile-dialog-test-icons
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES
            icons/edit.svg
            icons/location-pin.svg
    )
    if(MSVC)
        target_compile_options(
            shadow-personal-profile-dialog-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-personal-profile-dialog-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-personal-profile-dialog
        COMMAND shadow-personal-profile-dialog-test
    )
    set_tests_properties(
        shadow-desktop-personal-profile-dialog
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-review-travel-collections-test
        tests/review_travel_collections_test.cpp
    )
    target_compile_features(shadow-review-travel-collections-test PRIVATE cxx_std_20)
    target_link_libraries(
        shadow-review-travel-collections-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-review-travel-collections-test
        URI Shadow.TravelCollectionsContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/ReviewTravelCollections.qml
            qml/ShadowIcon.qml
            qml/Theme.qml
    )
    qt_add_resources(
        shadow-review-travel-collections-test
        shadow-review-travel-collections-test-icons
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES icons/map.svg
    )
    if(MSVC)
        target_compile_options(
            shadow-review-travel-collections-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-travel-collections-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-travel-collections
        COMMAND shadow-review-travel-collections-test
    )
    set_tests_properties(
        shadow-desktop-review-travel-collections
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

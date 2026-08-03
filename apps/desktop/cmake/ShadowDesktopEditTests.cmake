# Precision edit-session, preview/detail transport, tool interaction, and
# packaged edit-component contracts.
    add_executable(
        shadow-edit-fine-parameter-registry-test
        tests/edit_fine_parameter_registry_test.cpp
        src/edit_fine_parameter_registry.cpp
    )
    target_compile_features(
        shadow-edit-fine-parameter-registry-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-fine-parameter-registry-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-fine-parameter-registry-test
        PRIVATE Qt6::Core Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-fine-parameter-registry-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-fine-parameter-registry-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-fine-parameter-registry
        COMMAND shadow-edit-fine-parameter-registry-test
    )

    add_executable(
        shadow-preview-diagnostics-test
        tests/preview_diagnostics_test.cpp
        src/preview_diagnostics.cpp
        src/preview_diagnostics.hpp
    )
    target_compile_features(shadow-preview-diagnostics-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-preview-diagnostics-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-preview-diagnostics-test PRIVATE Qt6::Gui)
    if(MSVC)
        target_compile_options(shadow-preview-diagnostics-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-preview-diagnostics-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-preview-diagnostics COMMAND shadow-preview-diagnostics-test)

    add_executable(shadow-edit-history-test tests/edit_history_test.cpp)
    target_compile_features(shadow-edit-history-test PRIVATE cxx_std_20)
    target_include_directories(shadow-edit-history-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(MSVC)
        target_compile_options(shadow-edit-history-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-edit-history-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-edit-history COMMAND shadow-edit-history-test)

    add_executable(
        shadow-edit-history-restore-projection-test
        tests/edit_history_restore_projection_test.cpp
        src/edit_history_restore_projection.cpp
        src/edit_history_restore_projection.hpp
        src/edit_stack.hpp
    )
    target_compile_features(
        shadow-edit-history-restore-projection-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-history-restore-projection-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-history-restore-projection-test
        PRIVATE Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-history-restore-projection-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-history-restore-projection-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-history-restore-projection
        COMMAND shadow-edit-history-restore-projection-test
    )

    add_executable(
        shadow-edit-stack-test
        tests/edit_stack_test.cpp
        src/edit_stack.hpp
    )
    target_compile_features(shadow-edit-stack-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-edit-stack-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-edit-stack-test PRIVATE Qt6::Gui)
    if(MSVC)
        target_compile_options(shadow-edit-stack-test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            shadow-edit-stack-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(NAME shadow-desktop-edit-stack COMMAND shadow-edit-stack-test)

    add_executable(
        shadow-lut-preview-provider-test
        tests/lut_preview_provider_test.cpp
        src/lut_preview_provider.cpp
        src/lut_preview_provider.hpp
    )
    target_compile_features(shadow-lut-preview-provider-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-lut-preview-provider-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-lut-preview-provider-test
        PRIVATE Qt6::Quick Shadow::Image
    )
    if(MSVC)
        target_compile_options(
            shadow-lut-preview-provider-test PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-lut-preview-provider-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-lut-preview-provider
        COMMAND shadow-lut-preview-provider-test
    )

    add_executable(
        shadow-tone-curve-point-model-test
        tests/tone_curve_point_model_test.cpp
        src/tone_curve_point_model.cpp
        src/tone_curve_point_model.hpp
    )
    target_compile_features(shadow-tone-curve-point-model-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-tone-curve-point-model-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-tone-curve-point-model-test PRIVATE Qt6::Core)
    if(MSVC)
        target_compile_options(
            shadow-tone-curve-point-model-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-tone-curve-point-model-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-tone-curve-point-model
        COMMAND shadow-tone-curve-point-model-test
    )

    add_executable(
        shadow-edit-version-presentation-test
        tests/edit_version_presentation_test.cpp
        src/edit_version_model.cpp
        src/edit_version_presentation.cpp
    )
    target_compile_features(shadow-edit-version-presentation-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-edit-version-presentation-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-edit-version-presentation-test PRIVATE Qt6::Gui)
    if(MSVC)
        target_compile_options(
            shadow-edit-version-presentation-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-version-presentation-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-version-presentation
        COMMAND shadow-edit-version-presentation-test
    )

    add_executable(
        shadow-edit-preview-contract-test
        tests/edit_preview_contract_test.cpp
        src/edit_mask_coverage_store.cpp
        ${SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE}
        src/edit_preview_presentation_context.cpp
        src/edit_preview_provider.cpp
        src/edit_preview_store.cpp
    )
    target_compile_features(shadow-edit-preview-contract-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-edit-preview-contract-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-edit-preview-contract-test PRIVATE Qt6::Quick)
    if(MSVC)
        target_compile_options(
            shadow-edit-preview-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-preview-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-preview-contract
        COMMAND shadow-edit-preview-contract-test
    )

    add_executable(
        shadow-edit-preview-presentation-context-contract-test
        tests/edit_preview_presentation_context_contract_test.cpp
        src/edit_preview_presentation_context.cpp
    )
    target_compile_features(
        shadow-edit-preview-presentation-context-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-preview-presentation-context-contract-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-preview-presentation-context-contract-test
        PRIVATE Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-preview-presentation-context-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-preview-presentation-context-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-preview-presentation-context-contract
        COMMAND shadow-edit-preview-presentation-context-contract-test
    )

    add_executable(
        shadow-edit-preview-texture-item-contract-test
        tests/edit_preview_texture_item_contract_test.cpp
        ${SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE}
        src/edit_preview_liquify_mesh.cpp
        src/edit_preview_presentation_context.cpp
        src/edit_preview_presentation_registry.cpp
        src/edit_preview_store.cpp
        src/edit_preview_texture_item.cpp
    )
    target_compile_features(
        shadow-edit-preview-texture-item-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-preview-texture-item-contract-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-preview-texture-item-contract-test
        PRIVATE Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-preview-texture-item-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-preview-texture-item-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-preview-texture-item-contract
        COMMAND shadow-edit-preview-texture-item-contract-test
    )

    add_executable(
        shadow-edit-preview-liquify-mesh-test
        tests/edit_preview_liquify_mesh_test.cpp
        src/edit_preview_liquify_mesh.cpp
    )
    target_compile_features(
        shadow-edit-preview-liquify-mesh-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-preview-liquify-mesh-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-preview-liquify-mesh-test
        PRIVATE Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-preview-liquify-mesh-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-preview-liquify-mesh-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-preview-liquify-mesh
        COMMAND shadow-edit-preview-liquify-mesh-test
    )

    add_executable(
        shadow-edit-mask-coverage-store-contract-test
        tests/edit_mask_coverage_store_contract_test.cpp
        src/edit_mask_coverage_store.cpp
        ${SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE}
        src/edit_preview_presentation_context.cpp
        src/edit_preview_provider.cpp
        src/edit_preview_store.cpp
    )
    target_compile_features(
        shadow-edit-mask-coverage-store-contract-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-mask-coverage-store-contract-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-mask-coverage-store-contract-test
        PRIVATE Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-mask-coverage-store-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-mask-coverage-store-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-mask-coverage-store-contract
        COMMAND shadow-edit-mask-coverage-store-contract-test
    )

    add_executable(
        shadow-backend-edit-settings-projection-test
        tests/backend_edit_settings_projection_test.cpp
        ${SHADOW_DESKTOP_EDIT_SETTINGS_PROJECTION_SOURCES}
    )
    target_compile_features(
        shadow-backend-edit-settings-projection-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-backend-edit-settings-projection-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-backend-edit-settings-projection-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-backend-edit-settings-projection-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-backend-edit-settings-projection-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-backend-edit-settings-projection-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-backend-edit-settings-projection
        COMMAND shadow-backend-edit-settings-projection-test
    )

    add_executable(
        shadow-backend-raw-foundation-projection-test
        tests/backend_raw_foundation_projection_test.cpp
        src/backend/raw_foundation_projection.cpp
        src/backend/raw_foundation_projection.hpp
        src/desktop_backend.cpp
        src/desktop_backend.hpp
        src/desktop_backend_library.cpp
        src/desktop_backend_raw_foundation.cpp
        src/folder_scan_backend.cpp
        src/folder_scan_backend.hpp
        src/photo_inspection_projection.cpp
        src/photo_inspection_projection.hpp
        src/preview_diagnostics.cpp
        src/preview_diagnostics.hpp
    )
    target_compile_features(
        shadow-backend-raw-foundation-projection-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-backend-raw-foundation-projection-test
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${SHADOW_CXXBRIDGE_INCLUDE_DIRECTORY}"
    )
    target_link_libraries(
        shadow-backend-raw-foundation-projection-test
        PRIVATE shadow-desktop-export-backend
    )
    add_dependencies(
        shadow-backend-raw-foundation-projection-test
        shadow-desktop-rust-build
    )
    if(MSVC)
        target_compile_options(
            shadow-backend-raw-foundation-projection-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-backend-raw-foundation-projection-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-backend-raw-foundation-projection
        COMMAND shadow-backend-raw-foundation-projection-test
    )

    add_executable(
        shadow-edit-liquify-coordinates-test
        tests/edit_liquify_coordinates_test.cpp
        src/edit_liquify_coordinates.cpp
        src/edit_liquify_coordinates.hpp
    )
    target_compile_features(
        shadow-edit-liquify-coordinates-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-liquify-coordinates-test
        PRIVATE src
    )
    target_link_libraries(
        shadow-edit-liquify-coordinates-test
        PRIVATE Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-liquify-coordinates-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-liquify-coordinates-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-liquify-coordinates
        COMMAND shadow-edit-liquify-coordinates-test
    )

    add_executable(
        shadow-review-selection-state-contract-test
        tests/review_selection_state_contract_test.cpp
    )
    target_compile_features(
        shadow-review-selection-state-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-review-selection-state-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-review-selection-state-contract-test
        PRIVATE Qt6::Core Qt6::Qml
    )
    if(MSVC)
        target_compile_options(
            shadow-review-selection-state-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-selection-state-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-selection-state-contract
        COMMAND shadow-review-selection-state-contract-test
    )

    add_executable(
        shadow-review-comparison-state-contract-test
        tests/review_comparison_state_contract_test.cpp
    )
    target_compile_features(
        shadow-review-comparison-state-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-review-comparison-state-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-review-comparison-state-contract-test
        PRIVATE Qt6::Core Qt6::Qml
    )
    if(MSVC)
        target_compile_options(
            shadow-review-comparison-state-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-review-comparison-state-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-review-comparison-state-contract
        COMMAND shadow-review-comparison-state-contract-test
    )

    add_executable(
        shadow-autosave-failure-recovery-contract-test
        tests/autosave_failure_recovery_contract_test.cpp
    )
    target_compile_features(
        shadow-autosave-failure-recovery-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-autosave-failure-recovery-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-autosave-failure-recovery-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-autosave-failure-recovery-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-autosave-failure-recovery-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-autosave-failure-recovery-contract
        COMMAND shadow-autosave-failure-recovery-contract-test
    )
    set_tests_properties(
        shadow-desktop-autosave-failure-recovery-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-edit-persistence-task-coordinator-test
        tests/edit_persistence_task_coordinator_test.cpp
        src/edit_persistence_task_coordinator.cpp
        src/edit_persistence_task_coordinator.hpp
    )
    target_compile_features(
        shadow-edit-persistence-task-coordinator-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-persistence-task-coordinator-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        shadow-edit-persistence-task-coordinator-test
        PRIVATE Qt6::Core Qt6::Concurrent Qt6::Gui
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-persistence-task-coordinator-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-persistence-task-coordinator-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-persistence-task-coordinator
        COMMAND shadow-edit-persistence-task-coordinator-test
    )

    add_executable(
        shadow-precision-lut-section-contract-test
        tests/precision_lut_section_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-lut-section-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-precision-lut-section-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-precision-lut-section-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-lut-section-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-lut-section-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-lut-section-contract
        COMMAND shadow-precision-lut-section-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-lut-section-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-mask-create-menu-contract-test
        tests/precision_mask_create_menu_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-mask-create-menu-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-precision-mask-create-menu-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-precision-mask-create-menu-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-mask-create-menu-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-mask-create-menu-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-mask-create-menu-contract
        COMMAND shadow-precision-mask-create-menu-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-mask-create-menu-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-mask-coverage-overlay-contract-test
        tests/precision_mask_coverage_overlay_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-mask-coverage-overlay-contract-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-precision-mask-coverage-overlay-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    qt_add_qml_module(
        shadow-precision-mask-coverage-overlay-contract-test
        URI Shadow.MaskCoverageContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PrecisionMaskCoverageOverlay.qml
            qml/Theme.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-mask-coverage-overlay-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-mask-coverage-overlay-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-mask-coverage-overlay-contract
        COMMAND shadow-precision-mask-coverage-overlay-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-mask-coverage-overlay-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-retouch-tools-contract-test
        tests/precision_retouch_tools_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-retouch-tools-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-precision-retouch-tools-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-precision-retouch-tools-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-retouch-tools-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-retouch-tools-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-retouch-tools-contract
        COMMAND shadow-precision-retouch-tools-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-retouch-tools-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-edit-ai-mask-prompt-state-test
        tests/edit_ai_mask_prompt_state_test.cpp
        src/edit_ai_mask_prompt_state.cpp
        src/edit_ai_mask_prompt_state.hpp
    )
    target_compile_features(
        shadow-edit-ai-mask-prompt-state-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-ai-mask-prompt-state-test
        PRIVATE src
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-ai-mask-prompt-state-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-ai-mask-prompt-state-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-ai-mask-prompt-state
        COMMAND shadow-edit-ai-mask-prompt-state-test
    )

    add_executable(
        shadow-edit-raw-foundation-state-test
        tests/edit_raw_foundation_state_test.cpp
        src/edit_raw_foundation_state.cpp
        src/edit_raw_foundation_state.hpp
    )
    target_compile_features(
        shadow-edit-raw-foundation-state-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-edit-raw-foundation-state-test
        PRIVATE src
    )
    if(MSVC)
        target_compile_options(
            shadow-edit-raw-foundation-state-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-edit-raw-foundation-state-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-edit-raw-foundation-state
        COMMAND shadow-edit-raw-foundation-state-test
    )

    add_executable(
        shadow-precision-ai-mask-prompt-overlay-contract-test
        tests/precision_ai_mask_prompt_overlay_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-ai-mask-prompt-overlay-contract-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-precision-ai-mask-prompt-overlay-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    qt_add_qml_module(
        shadow-precision-ai-mask-prompt-overlay-contract-test
        URI Shadow.AiMaskPromptOverlayContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PrecisionAiMaskPromptOverlay.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-ai-mask-prompt-overlay-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-ai-mask-prompt-overlay-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-ai-mask-prompt-overlay-contract
        COMMAND shadow-precision-ai-mask-prompt-overlay-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-ai-mask-prompt-overlay-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-detail-loupe-contract-test
        tests/precision_detail_loupe_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-detail-loupe-contract-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-precision-detail-loupe-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-precision-detail-loupe-contract-test
        URI Shadow.DetailLoupeContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PrecisionDetailLoupe.qml
            qml/PrecisionDetailLoupeState.qml
            qml/ShadowButton.qml
            qml/ShadowIcon.qml
            qml/ShadowIconButton.qml
            qml/Theme.qml
    )
    qt_add_resources(
        shadow-precision-detail-loupe-contract-test
        shadow-precision-detail-loupe-contract-test-icons
        PREFIX "/icons"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
        FILES
            icons/close.svg
            icons/pin.svg
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-detail-loupe-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-detail-loupe-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-detail-loupe-contract
        COMMAND shadow-precision-detail-loupe-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-detail-loupe-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-raw-foundation-adjustments-contract-test
        tests/precision_raw_foundation_adjustments_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-raw-foundation-adjustments-contract-test
        PRIVATE cxx_std_20
    )
    target_link_libraries(
        shadow-precision-raw-foundation-adjustments-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    qt_add_qml_module(
        shadow-precision-raw-foundation-adjustments-contract-test
        URI Shadow.RawFoundationAdjustmentsContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PrecisionFoundationAdjustments.qml
            qml/PrecisionRawDenoiseAdjustments.qml
            qml/ShadowAdjustmentSection.qml
            qml/ShadowButton.qml
            qml/ShadowCheckBox.qml
            qml/ShadowIcon.qml
            qml/ShadowIconButton.qml
            qml/ShadowInlineSlider.qml
            qml/ShadowSlider.qml
            qml/Theme.qml
            qml/ToneCurveEditor.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-raw-foundation-adjustments-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-raw-foundation-adjustments-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-raw-foundation-adjustments-contract
        COMMAND shadow-precision-raw-foundation-adjustments-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-raw-foundation-adjustments-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-adjustment-controls-contract-test
        tests/shadow_adjustment_controls_contract_test.cpp
    )
    target_compile_features(
        shadow-adjustment-controls-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-adjustment-controls-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-adjustment-controls-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    )
    if(MSVC)
        target_compile_options(
            shadow-adjustment-controls-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-adjustment-controls-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-adjustment-controls-contract
        COMMAND shadow-adjustment-controls-contract-test
    )
    set_tests_properties(
        shadow-desktop-adjustment-controls-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-direct-stroke-interaction-test
        tests/precision_direct_stroke_interaction_test.cpp
        src/edit_stroke_input.cpp
        src/edit_stroke_input.hpp
    )
    target_compile_features(
        shadow-precision-direct-stroke-interaction-test
        PRIVATE cxx_std_20
    )
    target_include_directories(
        shadow-precision-direct-stroke-interaction-test
        PRIVATE src
    )
    target_link_libraries(
        shadow-precision-direct-stroke-interaction-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    qt_add_qml_module(
        shadow-precision-direct-stroke-interaction-test
        URI Shadow.DirectStrokeContract
        VERSION 1.0
        RESOURCE_PREFIX "/qt/qml"
        NO_PLUGIN
        QML_FILES
            qml/PrecisionActiveStrokeCoverage.qml
            qml/PrecisionCanvasZoomInput.qml
            qml/PrecisionCanvasPickerInput.qml
            qml/PrecisionCropOverlay.qml
            qml/PrecisionLiquifyOverlay.qml
            qml/PrecisionLocalMaskOverlay.qml
            qml/ShadowIcon.qml
            qml/Theme.qml
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-direct-stroke-interaction-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-direct-stroke-interaction-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-direct-stroke-interaction
        COMMAND shadow-precision-direct-stroke-interaction-test
    )
    set_tests_properties(
        shadow-desktop-precision-direct-stroke-interaction
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

    add_executable(
        shadow-precision-point-color-section-contract-test
        tests/precision_point_color_section_contract_test.cpp
    )
    target_compile_features(
        shadow-precision-point-color-section-contract-test
        PRIVATE cxx_std_20
    )
    target_compile_definitions(
        shadow-precision-point-color-section-contract-test
        PRIVATE SHADOW_DESKTOP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(
        shadow-precision-point-color-section-contract-test
        PRIVATE Qt6::Gui Qt6::Qml Qt6::Quick
    )
    if(MSVC)
        target_compile_options(
            shadow-precision-point-color-section-contract-test
            PRIVATE /W4 /permissive-
        )
    else()
        target_compile_options(
            shadow-precision-point-color-section-contract-test
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
    endif()
    add_test(
        NAME shadow-desktop-precision-point-color-section-contract
        COMMAND shadow-precision-point-color-section-contract-test
    )
    set_tests_properties(
        shadow-desktop-precision-point-color-section-contract
        PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )

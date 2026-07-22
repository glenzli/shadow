find_package(
    Qt6 6.11.1 REQUIRED
    COMPONENTS Core Concurrent LinguistTools Quick QuickControls2 Svg
)
find_package(PkgConfig REQUIRED)
find_package(JPEG REQUIRED)
find_package(Iconv REQUIRED)
pkg_check_modules(LibRaw REQUIRED IMPORTED_TARGET libraw)
pkg_check_modules(Lensfun QUIET IMPORTED_TARGET lensfun)
pkg_check_modules(LittleCms REQUIRED IMPORTED_TARGET lcms2)

if(APPLE)
    enable_language(OBJCXX)
endif()

qt_standard_project_setup(
    REQUIRES 6.11
    I18N_SOURCE_LANGUAGE en
    I18N_TRANSLATED_LANGUAGES zh_CN
)

find_program(SHADOW_CARGO_EXECUTABLE cargo REQUIRED)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(SHADOW_CARGO_PROFILE release)
    set(SHADOW_CARGO_PROFILE_ARGUMENT --release)
else()
    set(SHADOW_CARGO_PROFILE debug)
    set(SHADOW_CARGO_PROFILE_ARGUMENT)
endif()

file(
    GLOB_RECURSE SHADOW_RUST_BRIDGE_INPUTS
    CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/Cargo.toml"
    "${PROJECT_SOURCE_DIR}/Cargo.lock"
    "${PROJECT_SOURCE_DIR}/crates/*/Cargo.toml"
    "${PROJECT_SOURCE_DIR}/crates/*/build.rs"
    "${PROJECT_SOURCE_DIR}/crates/*/src/*.rs"
    "${PROJECT_SOURCE_DIR}/crates/*/src/**/*.rs"
    "${PROJECT_SOURCE_DIR}/cpp/shadow-image/include/**/*.hpp"
    "${PROJECT_SOURCE_DIR}/cpp/shadow-image/src/**/*.cpp"
    "${PROJECT_SOURCE_DIR}/cpp/shadow-image/src/**/*.hpp"
)

set(
    SHADOW_DESKTOP_RUST_LIBRARY
    "${PROJECT_SOURCE_DIR}/target/${SHADOW_CARGO_PROFILE}/libshadow_desktop_bridge.a"
)
set(SHADOW_CARGO_ENV "${CMAKE_COMMAND}" -E env)
if(SHADOW_LENSFUN_PKGCONFIG_PATH)
    # The Rust bridge compiles the same optical adapter independently. Keep its pkg-config view
    # aligned with CMake's optional Lensfun prefix; otherwise the GUI could link Lensfun while
    # the bridge silently compiled its no-op provider.
    list(APPEND SHADOW_CARGO_ENV "PKG_CONFIG_PATH=${SHADOW_LENSFUN_PKGCONFIG_PATH}")
endif()
if(APPLE)
    # Homebrew's Qt 6.11 frameworks currently target macOS 26.0. Match the
    # Rust static library so the final app does not raise mixed-minimum warnings.
    list(APPEND SHADOW_CARGO_ENV MACOSX_DEPLOYMENT_TARGET=26.0)
endif()
add_custom_command(
    OUTPUT "${SHADOW_DESKTOP_RUST_LIBRARY}"
    COMMAND
        ${SHADOW_CARGO_ENV} "${SHADOW_CARGO_EXECUTABLE}" build
        --package shadow-desktop-bridge
        ${SHADOW_CARGO_PROFILE_ARGUMENT}
        --target-dir "${PROJECT_SOURCE_DIR}/target"
    DEPENDS ${SHADOW_RUST_BRIDGE_INPUTS}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Building the Rust desktop snapshot bridge"
    VERBATIM
)
add_custom_target(shadow-desktop-rust-build DEPENDS "${SHADOW_DESKTOP_RUST_LIBRARY}")
add_library(shadow-desktop-rust STATIC IMPORTED GLOBAL)
set_target_properties(
    shadow-desktop-rust
    PROPERTIES IMPORTED_LOCATION "${SHADOW_DESKTOP_RUST_LIBRARY}"
)
add_dependencies(shadow-desktop-rust shadow-desktop-rust-build)

qt_add_executable(
    shadow-desktop
    MACOSX_BUNDLE
    src/desktop_backend.cpp
    src/desktop_backend.hpp
    src/edit_controller.cpp
    src/edit_controller.hpp
    src/edit_history.hpp
    src/edit_preview_contract.hpp
    src/edit_preview_provider.cpp
    src/edit_preview_provider.hpp
    src/edit_stack.hpp
    src/edit_version_model.cpp
    src/edit_version_model.hpp
    src/edit_version_presentation.cpp
    src/edit_version_presentation.hpp
    src/main.cpp
    src/lut_library.cpp
    src/lut_library.hpp
    src/lut_preview_provider.cpp
    src/lut_preview_provider.hpp
    src/review_controller.cpp
    src/review_controller.hpp
    src/review_decision_session.hpp
    src/review_evidence_session.hpp
    src/review_filter_model.cpp
    src/review_filter_model.hpp
    src/review_model.cpp
    src/review_model.hpp
    src/thumbnail_provider.cpp
    src/thumbnail_provider.hpp
    src/tone_curve_point_model.cpp
    src/tone_curve_point_model.hpp
    src/ui_preferences.cpp
    src/ui_preferences.hpp
)

qt_add_resources(
    shadow-desktop shadow-desktop-icons
    PREFIX "/icons"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
    FILES
        icons/add-folder.svg
        icons/before-after.svg
        icons/chevron-down.svg
        icons/check.svg
        icons/clear.svg
        icons/compare.svg
        icons/duplicate.svg
        icons/edit.svg
        icons/eyedropper.svg
        icons/fit-view.svg
        icons/filter.svg
        icons/library-manage.svg
        icons/metadata.svg
        icons/move-down.svg
        icons/move-up.svg
        icons/node-add.svg
        icons/pick.svg
        icons/redo.svg
        icons/reject.svg
        icons/review-grid.svg
        icons/settings.svg
        icons/slot-left.svg
        icons/slot-right.svg
        icons/star.svg
        icons/star-filled.svg
        icons/trash.svg
        icons/unflag.svg
        icons/undo.svg
)

qt_add_resources(
    shadow-desktop shadow-desktop-assets
    PREFIX "/assets"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    FILES
        assets/lut-preview-reference.jpg
)

if(APPLE)
    target_sources(
        shadow-desktop
        PRIVATE
            src/mac_titlebar.hpp
            src/mac_titlebar.mm
    )
    target_link_libraries(shadow-desktop PRIVATE "-framework AppKit")
endif()

set_source_files_properties(
    qml/Theme.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(
    shadow-desktop
    URI Shadow.App
    VERSION 1.0
    QML_FILES
        qml/EditHistogram.qml
        qml/LibraryWorkspace.qml
        qml/LutManagerWindow.qml
        qml/Main.qml
        qml/MetadataWindow.qml
        qml/OpticsProfileManagerWindow.qml
        qml/PreferencesMenu.qml
        qml/PrecisionWorkspace.qml
        qml/ReviewWorkspace.qml
        qml/ShadowButton.qml
        qml/ShadowColorLabelButton.qml
        qml/ShadowColorWheel.qml
        qml/ShadowAdjustmentSection.qml
        qml/ShadowHueRange.qml
        qml/ShadowIcon.qml
        qml/ShadowIconButton.qml
        qml/ShadowInlineSlider.qml
        qml/ShadowSlider.qml
        qml/ShadowTabButton.qml
        qml/Theme.qml
        qml/ToneCurveEditor.qml
)

qt_add_translations(
    TARGETS shadow-desktop
    SOURCE_TARGETS shadow-desktop
    TS_FILE_BASE shadow
    TS_FILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/i18n"
    RESOURCE_PREFIX "/i18n"
    LUPDATE_TARGET shadow-desktop-update-translations
    LUPDATE_OPTIONS -no-obsolete
    LRELEASE_TARGET shadow-desktop-release-translations
    LRELEASE_OPTIONS -nounfinished
    NO_GENERATE_PLURALS_TS_FILE
)

target_compile_features(shadow-desktop PRIVATE cxx_std_20)
target_include_directories(
    shadow-desktop
    PRIVATE
        "${PROJECT_SOURCE_DIR}/target/cxxbridge"
        "${PROJECT_SOURCE_DIR}/cpp/shadow-image/include"
)
target_link_libraries(
    shadow-desktop
    PRIVATE
        shadow-desktop-rust
        PkgConfig::LibRaw
        PkgConfig::LittleCms
        ${CMAKE_DL_LIBS}
        JPEG::JPEG
        Iconv::Iconv
        Qt6::Concurrent
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Svg
)
if(Lensfun_FOUND)
    target_link_libraries(shadow-desktop PRIVATE PkgConfig::Lensfun)
endif()

if(MSVC)
    target_compile_options(shadow-desktop PRIVATE /W4 /permissive-)
else()
    target_compile_options(
        shadow-desktop
        PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()

set_target_properties(
    shadow-desktop
    PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Shadow"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.shadow.photo"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "1"
        OUTPUT_NAME "Shadow"
)

if(BUILD_TESTING)
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
        shadow-edit-stack-test
        tests/edit_stack_test.cpp
        src/edit_stack.hpp
    )
    target_compile_features(shadow-edit-stack-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-edit-stack-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-edit-stack-test PRIVATE Qt6::Core)
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
    target_link_libraries(shadow-edit-version-presentation-test PRIVATE Qt6::Core)
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
        src/edit_preview_provider.cpp
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
        shadow-review-model-test
        tests/review_model_test.cpp
        src/review_model.cpp
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
        shadow-review-filter-model-test
        tests/review_filter_model_test.cpp
        src/review_filter_model.cpp
        src/review_filter_model.hpp
        src/review_model.cpp
        src/review_model.hpp
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
        shadow-review-decision-session-test
        tests/review_decision_session_test.cpp
        src/review_decision_session.hpp
    )
    target_compile_features(shadow-review-decision-session-test PRIVATE cxx_std_20)
    target_include_directories(
        shadow-review-decision-session-test
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(shadow-review-decision-session-test PRIVATE Qt6::Core)
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
endif()

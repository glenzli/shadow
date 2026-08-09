# Desktop platform composition belongs here so adding another operating-system
# implementation does not add conditionals throughout the application target.

set(
    SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_HEADER
    src/platform/edit_preview_texture_factory.hpp
)

if(APPLE AND SHADOW_ENABLE_METAL)
    set(
        SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE
        src/platform/macos/edit_preview_texture_factory.mm
    )
else()
    set(
        SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE
        src/platform/portable/edit_preview_texture_factory.cpp
    )
endif()

set(
    SHADOW_DESKTOP_PLATFORM_SOURCES
    ${SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_HEADER}
    ${SHADOW_EDIT_PREVIEW_TEXTURE_FACTORY_SOURCE}
)

if(APPLE)
    list(
        APPEND SHADOW_DESKTOP_PLATFORM_SOURCES
        src/platform/macos/titlebar.hpp
        src/platform/macos/titlebar.mm
    )
endif()

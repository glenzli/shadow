# Desktop platform boundary

This directory owns Qt presentation code whose implementation changes with the
operating system or graphics API. Cross-platform controllers and product policy
remain in their responsibility-named owners under `src/`.

- `edit_preview_texture_factory.hpp` is the platform-neutral Qt Quick texture contract.
- `macos/` owns AppKit and Metal implementations used only by macOS builds.
  Native preview ownership extends through both the Qt texture wrapper and completion of
  every Metal command buffer using it; scene-graph teardown must not release in-flight pixels.
- `portable/` owns the host-materialization fallback used when native Metal import is unavailable.

Add future Windows implementations under `windows/` and select them in
`cmake/ShadowDesktopPlatformSources.cmake`; do not fork the desktop application or QML tree.

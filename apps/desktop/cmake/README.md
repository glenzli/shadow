# Desktop CMake owners

`CMakeLists.txt` remains the application composition root. The files here own
independently changing build responsibilities:

- `ShadowDesktopPlatformSources.cmake` selects the platform presentation units.
- `ShadowDesktopTests.cmake` is the test registration index; its application,
  edit, review, and settings children own their focused targets.
- `sync_cxxbridge_headers.cmake` publishes generated CXX headers into the
  desktop build graph.
- `ShadowDesktopWindowsDeploy.cmake` packages both applications, isolated workers,
  Qt/QML imports, native DLL dependencies, and the offline city index together.
- `ShadowDesktopWindowsResources.cmake` generates the executable icon sizes from
  the canonical artwork and embeds executable and Qt window icons for both GUI applications.

The macOS bundle layout stays in the composition root; Windows deployment has its own owner.

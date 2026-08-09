# Desktop CMake owners

`CMakeLists.txt` remains the application composition root. The files here own
independently changing build responsibilities:

- `ShadowDesktopPlatformSources.cmake` selects the platform presentation units.
- `ShadowDesktopTests.cmake` is the test registration index; its application,
  edit, review, and settings children own their focused targets.
- `sync_cxxbridge_headers.cmake` publishes generated CXX headers into the
  desktop build graph.

Packaging stays in the composition root until Windows packaging exists and
there is a second concrete lifecycle to extract.

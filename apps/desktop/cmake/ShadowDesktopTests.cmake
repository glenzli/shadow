# Desktop test graph index. Keep target definitions and their CTest properties
# in the responsibility-named owner below rather than growing this router.
if(BUILD_TESTING)
    include(cmake/ShadowDesktopApplicationTests.cmake)
    include(cmake/ShadowDesktopSettingsTests.cmake)
    include(cmake/ShadowDesktopEditTests.cmake)
    include(cmake/ShadowDesktopReviewTests.cmake)
endif()

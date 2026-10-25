add_test(NAME shadow-desktop-rust-bridge-headers
    COMMAND "${CMAKE_COMMAND}"
        "-DSHADOW_HEADER_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/rust-bridge-headers-contract"
        "-DSHADOW_HEADER_SYNC_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/sync_cxxbridge_headers.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/rust_bridge_headers_contract.cmake")
set_tests_properties(shadow-desktop-rust-bridge-headers PROPERTIES
    TIMEOUT 30 LABELS "desktop;build;contract" RUN_SERIAL TRUE)

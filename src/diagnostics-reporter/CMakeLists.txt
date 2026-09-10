##########################################################################
# Optional local diagnostics reporter. This target is intentionally
# independent from normal RBus routing and is disabled by default.
##########################################################################

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_library(rbus_diagnostics_reporter_core STATIC
    diagnostics_protocol.c
    diagnostics_store.c)

target_link_libraries(rbus_diagnostics_reporter_core -pthread)

add_executable(rbus-diagnostics-reporter
    reporter_main.c)

target_link_libraries(rbus-diagnostics-reporter
    rbus_diagnostics_reporter_core
    -pthread)

install(TARGETS rbus-diagnostics-reporter
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
    ${PROJECT_SOURCE_DIR}/conf/rbus-diagnostics-reporter.service
    DESTINATION lib/systemd/system)

if (ENABLE_UNIT_TESTING)
    # This binary is never installed. Its override makes the live socket test
    # independent of the production-owned /run/rbus-diagnostics directory.
    add_executable(rbus-diagnostics-reporter-test-server
        reporter_main.c)
    target_compile_definitions(rbus-diagnostics-reporter-test-server PRIVATE
        RBUS_DIAGNOSTICS_TEST_RUNTIME_DIRECTORY_OVERRIDE=1)
    target_link_libraries(rbus-diagnostics-reporter-test-server
        rbus_diagnostics_reporter_core
        -pthread)

    add_executable(rbus_diagnostics_reporter_test
        diagnostics_reporter_test.c)
    target_compile_definitions(rbus_diagnostics_reporter_test PRIVATE _GNU_SOURCE)
    target_link_libraries(rbus_diagnostics_reporter_test
        rbus_diagnostics_reporter_core
        -pthread)
    add_test(NAME rbus_diagnostics_reporter_test
        COMMAND rbus_diagnostics_reporter_test
            $<TARGET_FILE:rbus-diagnostics-reporter-test-server>)
endif ()

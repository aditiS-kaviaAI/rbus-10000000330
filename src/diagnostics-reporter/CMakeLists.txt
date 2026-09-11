add_library(rbus_diagnostics_reporter_core STATIC
    DiagnosticsProtocol.cpp
    DiagnosticsStore.cpp)

target_include_directories(rbus_diagnostics_reporter_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(rbus_diagnostics_reporter_core -pthread)

add_executable(rbus-diagnostics-reporter
    ReporterServer.cpp
    main.cpp)

target_link_libraries(rbus-diagnostics-reporter
    rbus_diagnostics_reporter_core
    -pthread)

add_executable(rbus-diagnostics-reporter-tests
    DiagnosticsReporterTest.cpp
    ReporterServer.cpp)

target_link_libraries(rbus-diagnostics-reporter-tests
    rbus_diagnostics_reporter_core
    -pthread)

add_test(NAME rbus-diagnostics-reporter-tests
    COMMAND rbus-diagnostics-reporter-tests)

install(TARGETS rbus-diagnostics-reporter
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES
    ${PROJECT_SOURCE_DIR}/conf/rbus-diagnostics-reporter.service
    DESTINATION lib/systemd/system)

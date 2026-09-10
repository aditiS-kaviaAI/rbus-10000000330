add_executable(rbus-diagnostics-reporter
    rbus_diagnostics_reporter.c)

target_include_directories(rbus-diagnostics-reporter PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/rbus)

target_compile_definitions(rbus-diagnostics-reporter PRIVATE
    RBUS_DIAGNOSTICS_ENABLED
    RBUS_DIAGNOSTICS_OBSERVATION_SOCKET="${RBUS_DIAGNOSTICS_OBSERVATION_SOCKET}"
    RBUS_DIAGNOSTICS_CONTROL_SOCKET="${RBUS_DIAGNOSTICS_CONTROL_SOCKET}")

target_link_libraries(rbus-diagnostics-reporter PRIVATE pthread)

install(TARGETS rbus-diagnostics-reporter
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

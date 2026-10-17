# Registers one engine unit-test executable with CTest. Each test receives a
# private scratch directory as argv[1].
function(flow_engine_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE flow_engine)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    flow_target_warnings(${name})
    set(scratch "${CMAKE_CURRENT_BINARY_DIR}/scratch/${name}")
    add_test(NAME ${name} COMMAND ${name} "${scratch}")
endfunction()

flow_engine_test(fl_event_tests    engine/core/fl_event_tests.c)
flow_engine_test(fl_log_tests      engine/core/fl_log_tests.c)
flow_engine_test(fl_job_tests      engine/core/fl_job_tests.c)
flow_engine_test(fl_platform_tests engine/platform/fl_platform_tests.c)
flow_engine_test(fl_crash_tests    engine/platform/fl_crash_tests.c)

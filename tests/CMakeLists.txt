# Registers one engine unit-test executable with CTest.
function(flow_engine_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE flow_engine)
    flow_target_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

flow_engine_test(fl_event_tests engine/core/fl_event_tests.c)

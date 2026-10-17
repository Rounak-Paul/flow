cmake_minimum_required(VERSION 3.20)

if(APPLE)
    project(flow VERSION 0.1.0 LANGUAGES C CXX OBJC)
else()
    project(flow VERSION 0.1.0 LANGUAGES C CXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(FLOW_BUILD_TESTS "Build Flow's unit tests" ON)

# Matches causality: the Vulkan SDK's shaderc_combined only ships against the
# release DLL CRT, so every configuration must use it to link.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

set(FLOW_BIN_DIR "${PROJECT_SOURCE_DIR}/bin" CACHE PATH "Directory receiving the flow executable")

find_package(Threads REQUIRED)

set(FLOW_BUILD_TESTING_SAVED ${BUILD_TESTING})
set(BUILD_TESTING OFF)
add_subdirectory(vendors/causality)
set(BUILD_TESTING ${FLOW_BUILD_TESTING_SAVED})
unset(FLOW_BUILD_TESTING_SAVED)

# Applies Flow's warning policy to one of its own targets.
function(flow_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

# Engine: platform-independent core with no UI dependency.
file(GLOB_RECURSE FLOW_ENGINE_SOURCES CONFIGURE_DEPENDS src/engine/*.c)
add_library(flow_engine STATIC ${FLOW_ENGINE_SOURCES})
target_include_directories(flow_engine PUBLIC "${PROJECT_SOURCE_DIR}/src/engine")
target_link_libraries(flow_engine PUBLIC Threads::Threads)
# Strict C11 hides POSIX/OS extensions the platform layer needs.
target_compile_definitions(flow_engine PRIVATE
    $<$<PLATFORM_ID:Linux>:_GNU_SOURCE>
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE>
)
flow_target_warnings(flow_engine)

# Editor: causality-based UI on top of the engine.
file(GLOB_RECURSE FLOW_EDITOR_SOURCES CONFIGURE_DEPENDS src/editor/*.c)
add_library(flow_editor STATIC ${FLOW_EDITOR_SOURCES})
target_include_directories(flow_editor PUBLIC "${PROJECT_SOURCE_DIR}/src/editor")
target_link_libraries(flow_editor PUBLIC flow_engine PRIVATE causality)
# causality is linked statically into the executable, so its public symbols
# must resolve as plain declarations rather than DLL imports on Windows.
target_compile_definitions(flow_editor PRIVATE
    FLOW_VERSION="${PROJECT_VERSION}"
    $<$<PLATFORM_ID:Windows>:CA_API=>
)
flow_target_warnings(flow_editor)

add_executable(flow src/main.c)
target_link_libraries(flow PRIVATE flow_editor)
flow_target_warnings(flow)
# Export symbols so crash-report backtraces resolve function names on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(flow PROPERTIES ENABLE_EXPORTS ON)
endif()

set_target_properties(flow PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${FLOW_BIN_DIR}")
foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set_target_properties(flow PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${FLOW_BIN_DIR}")
endforeach()

if(FLOW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

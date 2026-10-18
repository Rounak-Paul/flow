# Lua 5.4.7 — vendored directly from the official https://www.lua.org/ftp/
# distribution (sha256 verified against the published checksum). No upstream
# CMake project exists for Lua, so this file builds the interpreter core as
# a static library for embedding. The standalone `lua`/`luac` CLI sources are
# not vendored — only the embeddable library is needed.

file(GLOB LUA_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

add_library(lua STATIC ${LUA_SOURCES})

target_include_directories(lua PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

set_target_properties(lua PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    target_compile_definitions(lua PRIVATE LUA_USE_MACOSX)
elseif(UNIX)
    target_compile_definitions(lua PRIVATE LUA_USE_LINUX)
    target_link_libraries(lua PRIVATE dl m)
endif()

if(MSVC)
    target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

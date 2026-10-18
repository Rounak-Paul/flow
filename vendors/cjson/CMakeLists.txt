# cJSON 1.7.18 — vendored sources (MIT); see README.
add_library(cjson STATIC cJSON.c)
target_include_directories(cjson PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
set_target_properties(cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_definitions(cjson PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

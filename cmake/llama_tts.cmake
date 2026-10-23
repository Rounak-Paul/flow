# Builds llama.cpp's llama-tts in Release and copies it next to the flow executable.
# Speech looks only beside the app (or at $FLOW_LLAMA_TTS). Nothing is taken from PATH.

include(ExternalProject)
include(ProcessorCount)
ProcessorCount(FLOW_LLAMA_TTS_JOBS)
if(FLOW_LLAMA_TTS_JOBS EQUAL 0)
    set(FLOW_LLAMA_TTS_JOBS 8)
endif()

set(FLOW_LLAMA_TTS_TAG "b10760" CACHE STRING "llama.cpp tag that provides llama-tts with Qwen3-TTS")
set(FLOW_LLAMA_TTS_BIN "${FLOW_BIN_DIR}/llama-tts${CMAKE_EXECUTABLE_SUFFIX}")

set(FLOW_LLAMA_TTS_ARGS
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DBUILD_SHARED_LIBS=OFF
    -DLLAMA_BUILD_COMMON=ON
    -DLLAMA_BUILD_TOOLS=ON
    -DLLAMA_BUILD_TESTS=OFF
    -DLLAMA_BUILD_EXAMPLES=OFF
    -DLLAMA_BUILD_SERVER=OFF
    -DLLAMA_BUILD_APP=OFF
    -DLLAMA_TOOLS_INSTALL=OFF
    -DLLAMA_FATAL_WARNINGS=OFF
    -DLLAMA_BUILD_BORINGSSL=ON
    -DGGML_NATIVE=OFF
    -DGGML_BACKEND_DL=OFF
)
if(APPLE)
    list(APPEND FLOW_LLAMA_TTS_ARGS -DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON)
    if(CMAKE_OSX_ARCHITECTURES)
        list(APPEND FLOW_LLAMA_TTS_ARGS "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
    endif()
endif()

ExternalProject_Add(llama_tts
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG ${FLOW_LLAMA_TTS_TAG}
    GIT_SHALLOW TRUE
    UPDATE_COMMAND ""
    CMAKE_ARGS ${FLOW_LLAMA_TTS_ARGS}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target llama-tts --parallel ${FLOW_LLAMA_TTS_JOBS}
    INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_if_different
        <BINARY_DIR>/bin/llama-tts${CMAKE_EXECUTABLE_SUFFIX}
        "${FLOW_LLAMA_TTS_BIN}"
    BUILD_BYPRODUCTS "${FLOW_LLAMA_TTS_BIN}"
    EXCLUDE_FROM_ALL TRUE
)

add_dependencies(flow llama_tts)

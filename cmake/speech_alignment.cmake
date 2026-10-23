# Package local speech analysis independently of the editor's renderer/ggml linkage.
include(ExternalProject)
ExternalProject_Add(flow_whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG v1.9.5
    GIT_SHALLOW TRUE
    UPDATE_COMMAND ""
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
        -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=ON -DWHISPER_BUILD_SERVER=OFF
        -DGGML_NATIVE=OFF -DGGML_METAL_EMBED_LIBRARY=ON
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target whisper-cli --parallel 4
    INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory "${FLOW_BIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        <BINARY_DIR>/bin/whisper-cli${CMAKE_EXECUTABLE_SUFFIX}
        "${FLOW_BIN_DIR}/whisper-cli${CMAKE_EXECUTABLE_SUFFIX}"
    BUILD_BYPRODUCTS "${FLOW_BIN_DIR}/whisper-cli${CMAKE_EXECUTABLE_SUFFIX}"
    EXCLUDE_FROM_ALL TRUE)
add_custom_command(OUTPUT "${FLOW_BIN_DIR}/models/ggml-base.bin"
    COMMAND ${CMAKE_COMMAND} "-DOUTPUT=${FLOW_BIN_DIR}/models/ggml-base.bin"
        -P "${PROJECT_SOURCE_DIR}/cmake/download_speech_model.cmake"
    DEPENDS "${PROJECT_SOURCE_DIR}/cmake/download_speech_model.cmake")
add_custom_target(flow_speech_model DEPENDS "${FLOW_BIN_DIR}/models/ggml-base.bin")
add_dependencies(flow flow_whisper flow_speech_model)

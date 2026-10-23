get_filename_component(folder "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${folder}")
file(DOWNLOAD "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin"
    "${OUTPUT}.download" TLS_VERIFY ON
    EXPECTED_HASH SHA256=60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe
    STATUS result)
list(GET result 0 code)
if(NOT code EQUAL 0)
    file(REMOVE "${OUTPUT}.download")
    message(FATAL_ERROR "Could not download the verified speech alignment model: ${result}")
endif()
file(RENAME "${OUTPUT}.download" "${OUTPUT}")

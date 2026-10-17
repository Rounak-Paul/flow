# Generates a C source exposing a binary file as a byte array.
# Inputs: INPUT (file), OUTPUT (generated .c), SYMBOL (C identifier prefix).
file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_length)
math(EXPR size "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
file(WRITE "${OUTPUT}.tmp"
"/* Generated from ${INPUT}; do not edit. */\n#include <stddef.h>\n"
"const unsigned char ${SYMBOL}[] = {\n${bytes}\n};\n"
"const size_t ${SYMBOL}_size = ${size};\n")
file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")

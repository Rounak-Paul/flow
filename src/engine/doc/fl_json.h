#pragma once
#include <cJSON.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FL_DOCUMENT_LIMIT (128u * 1024u * 1024u)
/* Checked JSON construction and decoding context; first diagnostic is retained. */
typedef struct Fl_Json { char error[256]; } Fl_Json;
/* Record a diagnostic; returns false for use in validation expressions. */
bool fl_json_fail(Fl_Json *context, const char *message);
/* Validate UTF-8 bytes, rejecting embedded NUL, overlong encodings and surrogate codepoints. */
bool fl_json_utf8_valid(const char *text, size_t size);
/* Parse a complete UTF-8 document, rejecting duplicate fields and excessive depth/size. */
cJSON *fl_json_parse(Fl_Json *context, const char *text, size_t size);
/* Add an owned value to object or array; releases value on failure. */
bool fl_json_add(Fl_Json *context, cJSON *parent, const char *key, cJSON *value);
/* Validate object fields against a space-separated exact field list; rejects unknown fields. */
bool fl_json_object(Fl_Json *context, const cJSON *object, const char *fields);
/* Require a member of the specified JSON type; returns NULL and records an error on failure. */
const cJSON *fl_json_member(Fl_Json *context, const cJSON *object, const char *key, int type);
/* Decode exact unsigned integer: decimal string or safe JSON integer, with no signs/whitespace. */
bool fl_json_uint(Fl_Json *context, const cJSON *value, uint64_t *out);
/* Decode exact signed integer with the same safe-number policy. */
bool fl_json_int(Fl_Json *context, const cJSON *value, int64_t *out);
/* Construct a decimal string without losing 64-bit precision. */
cJSON *fl_json_u64(uint64_t value);
cJSON *fl_json_i64(int64_t value);
/* Copy a required JSON string into owned storage. */
char *fl_json_string(Fl_Json *context, const cJSON *value);
/* Construct/decode finite binary64 values. */
bool fl_json_number(Fl_Json *context, const cJSON *value, double *out);
/* Format a checked JSON tree and atomically save it; tree remains caller-owned. */
bool fl_json_save(Fl_Json *context, const char *path, const cJSON *root);

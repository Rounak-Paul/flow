#include "fl_binary.h"
#include "platform/fl_fs.h"
#include <stdlib.h>
#include <string.h>

/* Hash bytes to detect truncation and accidental file corruption. */
static uint64_t checksum(const unsigned char *data, size_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * UINT64_C(1099511628211);
    return hash;
}

void fl_binary_write(Fl_Binary *s, const void *data, size_t size)
{
    if (s->failed) return;
    if (size > SIZE_MAX - s->size || (size && !data)) { s->failed = true; return; }
    size_t needed = s->size + size;
    if (needed > s->capacity) {
        size_t cap = s->capacity ? s->capacity : 256;
        while (cap < needed) { if (cap > SIZE_MAX / 2) { cap = needed; break; } cap *= 2; }
        void *next = realloc(s->data, cap);
        if (!next) { s->failed = true; return; }
        s->data = next; s->capacity = cap;
    }
    if (size) memcpy(s->data + s->size, data, size);
    s->size = needed;
}

void fl_binary_read(Fl_Binary *s, void *data, size_t size)
{
    if (s->failed || s->cursor > s->size || size > s->size - s->cursor) {
        s->failed = true; return;
    }
    if (size) memcpy(data, s->data + s->cursor, size);
    s->cursor += size;
}

void fl_binary_u64(Fl_Binary *s, uint64_t value)
{
    unsigned char bytes[8];
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (i * 8));
    fl_binary_write(s, bytes, sizeof(bytes));
}

uint64_t fl_binary_get_u64(Fl_Binary *s)
{
    unsigned char bytes[8] = {0};
    fl_binary_read(s, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (i * 8);
    return value;
}

void fl_binary_string(Fl_Binary *s, const char *text)
{
    size_t size = text ? strlen(text) : 0;
    fl_binary_u64(s, size); fl_binary_write(s, text, size);
}

char *fl_binary_get_string(Fl_Binary *s)
{
    uint64_t size = fl_binary_get_u64(s);
    if (s->failed || size >= SIZE_MAX || size > s->size - s->cursor) { s->failed = true; return NULL; }
    char *text = malloc((size_t)size + 1);
    if (!text) { s->failed = true; return NULL; }
    fl_binary_read(s, text, (size_t)size); text[size] = 0;
    if (memchr(text, 0, (size_t)size)) { free(text); s->failed = true; return NULL; }
    return text;
}

bool fl_binary_save(Fl_Binary *s, const char *path, const char magic[8])
{
    if (s->failed) return false;
    Fl_Binary file = {0};
    fl_binary_write(&file, magic, 8); fl_binary_u64(&file, 1);
    fl_binary_u64(&file, s->size); fl_binary_u64(&file, checksum(s->data, s->size));
    fl_binary_write(&file, s->data, s->size);
    bool ok = !file.failed && fl_fs_write_atomic(path, file.data, file.size);
    free(file.data); return ok;
}

bool fl_binary_load(Fl_Binary *s, const char *path, const char magic[8])
{
    *s = (Fl_Binary){0};
    s->data = (unsigned char *)fl_fs_read_file(path, SIZE_MAX - 1, &s->size);
    if (!s->data) return false;
    char found[8] = {0}; fl_binary_read(s, found, 8);
    uint64_t version = fl_binary_get_u64(s), size = fl_binary_get_u64(s), hash = fl_binary_get_u64(s);
    if (s->failed || memcmp(found, magic, 8) || version != 1 || size != s->size - s->cursor ||
        hash != checksum(s->data + s->cursor, (size_t)size)) {
        free(s->data); *s = (Fl_Binary){0}; return false;
    }
    return true;
}

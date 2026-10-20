#pragma once
#include "fl_json.h"
#include "platform/fl_paths.h"

/* A project-relative text replacement and the bytes observed before editing it. */
typedef struct Fl_DocumentWrite {
    const char *path;
    const char *text;
    size_t size;
    unsigned char expected[32];
    bool existed;
} Fl_DocumentWrite;
/* Hash a regular file; missing files are represented by existed=false, unreadable files fail. */
bool fl_document_fingerprint(const char *path, unsigned char hash[32], bool *existed);
/* Reject absolute/traversing paths; document writes stay within known project folders. */
bool fl_document_path_valid(const char *path);
/* Recover a prepared multi-file commit. Conflicting external bytes are never replaced. */
bool fl_document_recover(Fl_Json *context, const char *directory);
/* Validate expected file hashes, persist write intent, then finish all replacements. */
bool fl_document_commit(Fl_Json *context, const char *directory, const Fl_DocumentWrite *writes, size_t count);

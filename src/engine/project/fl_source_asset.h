#pragma once
#include "fl_project.h"

#define FL_SOURCE_NAME_MAX 64u

/* Validate a nonempty UTF-8 source asset name of at most FL_SOURCE_NAME_MAX bytes without controls. */
bool fl_source_asset_name_valid(const char *name);

/* Storage policy for an editable source asset. Media resources have no source format. */
typedef struct Fl_SourceFormat {
    const char *directory, *suffix, *extension;
    size_t limit;
} Fl_SourceFormat;
/* Borrow immutable storage policy for kind, or NULL for imported binary media. */
const Fl_SourceFormat *fl_source_asset_format(Fl_AssetKind kind);
/* Validate source encoding and kind-specific document syntax without resolving project dependencies. */
bool fl_source_asset_validate(Fl_AssetKind kind, const char *source, size_t size, char *error, size_t capacity);

/* Compiled project-wide resource references, derived from source and never persisted separately. */
typedef struct Fl_SourceInfo { Fl_Id *references; size_t reference_count; } Fl_SourceInfo;
/* Validate source and collect references into empty owned info; caller frees info.references. */
bool fl_source_asset_compile(Fl_AssetKind kind, const char *source, size_t size, Fl_SourceInfo *info, char *error, size_t capacity);
/* Produce a unique display name for a source import/duplicate without cutting UTF-8 codepoints. */
bool fl_source_asset_unique_name(const Fl_Project *project, Fl_AssetKind kind, const char *base, char out[FL_SOURCE_NAME_MAX+1]);

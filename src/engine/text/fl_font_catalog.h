#pragma once
#include <stdbool.h>
#include <stddef.h>

/* One scalable face; strings and bytes are owned by (or borrowed through) its catalog. */
typedef struct Fl_FontFace {
    const char *family, *style;
    const char *path;               /* file source; NULL for memory faces */
    const unsigned char *data;      /* borrowed memory source when path is NULL */
    size_t size;
    long index;                     /* FreeType face index, including named-instance bits */
    unsigned weight;                /* CSS weight 1..1000 */
    bool italic;
} Fl_FontFace;

/* Immutable, indexed set of faces; safe to read from any thread once created. */
typedef struct Fl_FontCatalog Fl_FontCatalog;

/* Index every scalable face in files (duplicates ignored); unreadable files are skipped. */
Fl_FontCatalog *fl_font_catalog_from_files(const char *const *paths, size_t count);
/* Index faces of borrowed font bytes that must outlive the catalog. */
Fl_FontCatalog *fl_font_catalog_from_memory(const unsigned char *data, size_t size);
/* Enumerate and index installed system fonts. Blocking; intended for a worker thread. */
Fl_FontCatalog *fl_font_catalog_scan_system(void);
/* Release a catalog; faces borrowed from it become invalid. NULL is safe. */
void fl_font_catalog_destroy(Fl_FontCatalog *catalog);
/* Count indexed faces (all families). */
size_t fl_font_catalog_face_count(const Fl_FontCatalog *catalog);
/* Unique family names sorted case-insensitively. */
size_t fl_font_catalog_family_count(const Fl_FontCatalog *catalog);
const char *fl_font_catalog_family(const Fl_FontCatalog *catalog, size_t index);
/* Borrow family's faces (ordered by weight, upright first); returns their count, zero when absent. */
size_t fl_font_catalog_faces(const Fl_FontCatalog *catalog, const char *family, const Fl_FontFace **faces);
/* Pick family's closest face using CSS font-matching rules; NULL when the family is absent. */
const Fl_FontFace *fl_font_catalog_match(const Fl_FontCatalog *catalog, const char *family, unsigned weight, bool italic);
/* Pick the closest face among count faces of one family with CSS weight/style rules; NULL when count is zero. */
const Fl_FontFace *fl_font_match_faces(const Fl_FontFace *faces, size_t count, unsigned weight, bool italic);
/* Compare ASCII case-insensitively, as family lookups do. */
int fl_font_family_compare(const char *a, const char *b);
/* Report whether path's first face is a scalable font FreeType can read. */
bool fl_font_file_valid(const char *path);

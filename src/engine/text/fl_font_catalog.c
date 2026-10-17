#include "fl_font_catalog.h"
#include "fl_freetype.h"
#include "platform/fl_font_paths.h"
#include FT_MULTIPLE_MASTERS_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H
#include FT_TRUETYPE_TABLES_H
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fl_FamilyRange { size_t first, count; } Fl_FamilyRange;
struct Fl_FontCatalog {
    Fl_FontFace *faces;
    size_t count, capacity;
    Fl_FamilyRange *families;
    size_t family_count;
};
typedef struct Fl_PathList { char **paths; size_t count, capacity; } Fl_PathList;

int fl_font_family_compare(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}

/* Duplicate text into owned storage. */
static char *copy_text(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

static bool contains(const char *text, const char *needle);

/* Rank normal-width styles ahead of condensed/expanded ones, as CSS font-stretch: normal prefers. */
static int width_rank(const char *style)
{
    static const char *widths[] = {"condensed", "compressed", "narrow", "extended", "expanded", "wide"};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) if (contains(style, widths[i])) return 1;
    return 0;
}

/* Order faces by family, weight, upright before italic, then normal width first. */
static int compare_faces(const void *x, const void *y)
{
    const Fl_FontFace *a = x, *b = y;
    int order = fl_font_family_compare(a->family, b->family);
    if (order) return order;
    if (a->weight != b->weight) return a->weight < b->weight ? -1 : 1;
    if (a->italic != b->italic) return (int)a->italic - (int)b->italic;
    return width_rank(a->style) - width_rank(b->style);
}

/* Append a face, taking ownership of its family/style/path strings. */
static bool append_face(Fl_FontCatalog *c, Fl_FontFace face)
{
    if (c->count == c->capacity) {
        size_t capacity = c->capacity ? c->capacity * 2 : 64;
        Fl_FontFace *faces = capacity < SIZE_MAX / sizeof(*faces) ? realloc(c->faces, capacity * sizeof(*faces)) : NULL;
        if (!faces) return false;
        c->faces = faces; c->capacity = capacity;
    }
    c->faces[c->count++] = face; return true;
}

/* Return whether text contains needle, ignoring ASCII case. */
static bool contains(const char *text, const char *needle)
{
    size_t n = strlen(needle);
    for (; text && *text; ++text) {
        size_t i = 0;
        while (i < n && text[i] && (text[i] | 32) == (needle[i] | 32)) ++i;
        if (i == n) return true;
    }
    return false;
}

/* Copy SFNT name id into UTF-8, preferring Windows Unicode English, then any Unicode or Mac Roman entry. */
static bool sfnt_name(FT_Face face, FT_UInt id, char *out, size_t capacity)
{
    FT_SfntName best = {0}, name;
    int best_rank = 0;
    for (FT_UInt i = 0, n = FT_Get_Sfnt_Name_Count(face); i < n; ++i) {
        if (FT_Get_Sfnt_Name(face, i, &name) || name.name_id != id) continue;
        bool unicode = (name.platform_id == TT_PLATFORM_MICROSOFT && (name.encoding_id == TT_MS_ID_UNICODE_CS || name.encoding_id == TT_MS_ID_UCS_4)) ||
                       name.platform_id == TT_PLATFORM_APPLE_UNICODE;
        int rank = unicode ? (name.language_id == TT_MS_LANGID_ENGLISH_UNITED_STATES ? 3 : 2) :
                   name.platform_id == TT_PLATFORM_MACINTOSH && name.encoding_id == TT_MAC_ID_ROMAN ? 1 : 0;
        if (rank > best_rank) { best = name; best_rank = rank; }
    }
    if (!best_rank || capacity < 2) return false;
    size_t at = 0;
    if (best_rank == 1) {
        for (FT_UInt i = 0; i < best.string_len && at + 1 < capacity; ++i) if (best.string[i] && best.string[i] < 128) out[at++] = (char)best.string[i];
    } else {
        for (FT_UInt i = 0; i + 1 < best.string_len; i += 2) {
            uint32_t c = (uint32_t)best.string[i] << 8 | best.string[i + 1];
            if (c >= 0xd800 && c < 0xdc00 && i + 3 < best.string_len) {
                uint32_t low = (uint32_t)best.string[i + 2] << 8 | best.string[i + 3];
                if (low >= 0xdc00 && low < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00); i += 2; }
            }
            if (!c || (c >= 0xd800 && c < 0xe000)) continue;
            unsigned char bytes[4]; size_t n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
            if (n == 1) bytes[0] = (unsigned char)c;
            else {
                for (size_t k = n - 1; k > 0; --k) { bytes[k] = (unsigned char)(0x80 | (c & 0x3f)); c >>= 6; }
                bytes[0] = (unsigned char)((n == 2 ? 0xc0 : n == 3 ? 0xe0 : 0xf0) | c);
            }
            if (at + n >= capacity) break;
            memcpy(out + at, bytes, n); at += n;
        }
    }
    out[at] = 0;
    return at > 0;
}

/* Build an owned face entry; false (with nothing retained) on allocation failure. */
static bool make_face(const char *family, const char *style, const char *path, const unsigned char *data, size_t size,
                      long index, unsigned weight, bool italic, Fl_FontFace *out)
{
    *out = (Fl_FontFace){.family = copy_text(family), .style = copy_text(style && style[0] ? style : "Regular"),
        .path = path ? copy_text(path) : NULL, .data = data, .size = size, .index = index,
        .weight = weight >= 1 && weight <= 1000 ? weight : 400, .italic = italic};
    if (out->family && out->style && (!path || out->path)) return true;
    free((void *)out->family); free((void *)out->style); free((void *)out->path);
    return false;
}

/* Append a face, releasing its strings when the catalog cannot grow. */
static void add_face(Fl_FontCatalog *c, Fl_FontFace face)
{
    if (!append_face(c, face)) { free((void *)face.family); free((void *)face.style); free((void *)face.path); }
}

/* Index the named instances of a variable face from its variation table, without reopening the font. */
static void add_instances(Fl_FontCatalog *c, FT_Library library, FT_Face face, const char *path,
                          const unsigned char *data, size_t size, long index, unsigned base_weight)
{
    FT_MM_Var *mm = NULL;
    if (FT_Get_MM_Var(face, &mm) || !mm) return;
    long wght = -1, ital = -1, slnt = -1;
    for (FT_UInt i = 0; i < mm->num_axis; ++i) {
        if (mm->axis[i].tag == FT_MAKE_TAG('w','g','h','t')) wght = (long)i;
        else if (mm->axis[i].tag == FT_MAKE_TAG('i','t','a','l')) ital = (long)i;
        else if (mm->axis[i].tag == FT_MAKE_TAG('s','l','n','t')) slnt = (long)i;
    }
    for (FT_UInt j = 0; j < mm->num_namedstyles && j < 0x7fff; ++j) {
        const FT_Var_Named_Style *named = &mm->namedstyle[j];
        char style[128];
        if (!sfnt_name(face, named->strid, style, sizeof(style))) snprintf(style, sizeof(style), "Instance %u", j + 1);
        double design = wght >= 0 ? named->coords[wght] / 65536.0 : 0;
        /* Apple's legacy 'fvar' weight axis is a 0.5..3 multiplier of regular (400) rather than CSS units. */
        if (wght >= 0 && mm->axis[wght].maximum <= 10 * 65536) design *= 400;
        unsigned weight = wght >= 0 ? (unsigned)fmin(1000, fmax(1, lround(design))) : base_weight;
        bool italic = (ital >= 0 && named->coords[ital] != 0) || (slnt >= 0 && named->coords[slnt] != 0) ||
                      contains(style, "italic") || contains(style, "oblique");
        Fl_FontFace entry;
        if (make_face(face->family_name, style, path, data, size, ((long)(j + 1) << 16) | index, weight, italic, &entry)) add_face(c, entry);
    }
    FT_Done_MM_Var(library, mm);
}

/* Add every visible scalable face and named variable instance of one source. */
static void add_source(Fl_FontCatalog *c, FT_Library library, const char *path, const unsigned char *data, size_t size)
{
    FT_Face face;
    if (fl_freetype_open(library, path, data, size, -1, &face)) return;
    long faces = face->num_faces;
    FT_Done_Face(face);
    for (long i = 0; i < faces && i < 0xffff; ++i) {
        if (fl_freetype_open(library, path, data, size, i, &face)) continue;
        if (face->family_name && face->family_name[0] && face->family_name[0] != '.' && FT_IS_SCALABLE(face)) {
            TT_OS2 *os2 = FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
            bool table = os2 && os2->version != 0xffff;
            unsigned weight = table ? os2->usWeightClass : 0;
            if (weight && weight < 10) weight *= 100;
            if (!weight || weight > 1000) weight = (face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400;
            bool italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) || (table && (os2->fsSelection & 0x201)) ||
                          contains(face->style_name, "italic") || contains(face->style_name, "oblique");
            Fl_FontFace entry;
            if (FT_HAS_MULTIPLE_MASTERS(face) && (face->style_flags >> 16) > 0) add_instances(c, library, face, path, data, size, i, weight);
            else if (make_face(face->family_name, face->style_name, path, data, size, i, weight, italic, &entry)) add_face(c, entry);
        }
        FT_Done_Face(face);
    }
}

/* Sort faces and derive contiguous family ranges. */
static Fl_FontCatalog *seal(Fl_FontCatalog *c)
{
    if (c->count) qsort(c->faces, c->count, sizeof(*c->faces), compare_faces);
    size_t families = 0;
    for (size_t i = 0; i < c->count; ++i) families += !i || fl_font_family_compare(c->faces[i - 1].family, c->faces[i].family);
    c->families = families ? malloc(families * sizeof(*c->families)) : NULL;
    if (families && !c->families) { fl_font_catalog_destroy(c); return NULL; }
    for (size_t i = 0; i < c->count; ++i) {
        if (!i || fl_font_family_compare(c->faces[i - 1].family, c->faces[i].family))
            c->families[c->family_count++] = (Fl_FamilyRange){i, 0};
        ++c->families[c->family_count - 1].count;
    }
    return c;
}

/* Order paths bytewise for duplicate removal. */
static int compare_paths(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

Fl_FontCatalog *fl_font_catalog_from_files(const char *const *paths, size_t count)
{
    if (count && !paths) return NULL;
    Fl_FontCatalog *c = calloc(1, sizeof(*c));
    FT_Library library = NULL;
    if (!c || FT_Init_FreeType(&library)) { free(c); return NULL; }
    const char **sorted = count ? malloc(count * sizeof(*sorted)) : NULL;
    if (count && !sorted) { FT_Done_FreeType(library); free(c); return NULL; }
    size_t unique = 0;
    for (size_t i = 0; i < count; ++i) if (paths[i]) sorted[unique++] = paths[i];
    if (unique) qsort(sorted, unique, sizeof(*sorted), compare_paths);
    for (size_t i = 0; i < unique; ++i)
        if (!i || strcmp(sorted[i - 1], sorted[i])) add_source(c, library, sorted[i], NULL, 0);
    free(sorted);
    FT_Done_FreeType(library);
    return seal(c);
}

Fl_FontCatalog *fl_font_catalog_from_memory(const unsigned char *data, size_t size)
{
    if (!data || !size) return NULL;
    Fl_FontCatalog *c = calloc(1, sizeof(*c));
    FT_Library library = NULL;
    if (!c || FT_Init_FreeType(&library)) { free(c); return NULL; }
    add_source(c, library, NULL, data, size);
    FT_Done_FreeType(library);
    return seal(c);
}

/* Collect enumerated system paths into an owned list. */
static void collect_path(const char *path, void *data)
{
    Fl_PathList *list = data;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 256;
        char **paths = capacity < SIZE_MAX / sizeof(*paths) ? realloc(list->paths, capacity * sizeof(*paths)) : NULL;
        if (!paths) return;
        list->paths = paths; list->capacity = capacity;
    }
    char *copy = copy_text(path);
    if (copy) list->paths[list->count++] = copy;
}

Fl_FontCatalog *fl_font_catalog_scan_system(void)
{
    Fl_PathList list = {0};
    fl_font_paths_list(collect_path, &list);
    Fl_FontCatalog *catalog = fl_font_catalog_from_files((const char *const *)list.paths, list.count);
    for (size_t i = 0; i < list.count; ++i) free(list.paths[i]);
    free(list.paths);
    return catalog;
}

void fl_font_catalog_destroy(Fl_FontCatalog *c)
{
    if (!c) return;
    for (size_t i = 0; i < c->count; ++i) {
        free((void *)c->faces[i].family); free((void *)c->faces[i].style); free((void *)c->faces[i].path);
    }
    free(c->faces); free(c->families); free(c);
}

size_t fl_font_catalog_face_count(const Fl_FontCatalog *c) { return c ? c->count : 0; }
size_t fl_font_catalog_family_count(const Fl_FontCatalog *c) { return c ? c->family_count : 0; }
const char *fl_font_catalog_family(const Fl_FontCatalog *c, size_t i)
{
    return c && i < c->family_count ? c->faces[c->families[i].first].family : NULL;
}

size_t fl_font_catalog_faces(const Fl_FontCatalog *c, const char *family, const Fl_FontFace **faces)
{
    if (faces) *faces = NULL;
    if (!c || !family) return 0;
    size_t low = 0, high = c->family_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int order = fl_font_family_compare(c->faces[c->families[mid].first].family, family);
        if (!order) {
            if (faces) *faces = &c->faces[c->families[mid].first];
            return c->families[mid].count;
        }
        if (order < 0) low = mid + 1; else high = mid;
    }
    return 0;
}

const Fl_FontFace *fl_font_match_faces(const Fl_FontFace *faces, size_t count, unsigned weight, bool italic)
{
    if (!faces || !count) return NULL;
    bool style_available = false;
    for (size_t i = 0; i < count; ++i) style_available |= faces[i].italic == italic;
    const Fl_FontFace *best = NULL;
    unsigned best_group = UINT32_MAX, best_distance = UINT32_MAX;
    for (size_t i = 0; i < count; ++i) {
        const Fl_FontFace *f = &faces[i];
        if (style_available && f->italic != italic) continue;
        unsigned w = f->weight, group, distance = w > weight ? w - weight : weight - w;
        if (weight >= 400 && weight <= 500) group = w >= weight && w <= 500 ? 0 : w < weight ? 1 : 2;
        else if (weight < 400) group = w <= weight ? 0 : 1;
        else group = w >= weight ? 0 : 1;
        if (group < best_group || (group == best_group && distance < best_distance)) {
            best = f; best_group = group; best_distance = distance;
        }
    }
    return best;
}

const Fl_FontFace *fl_font_catalog_match(const Fl_FontCatalog *c, const char *family, unsigned weight, bool italic)
{
    const Fl_FontFace *faces;
    size_t count = fl_font_catalog_faces(c, family, &faces);
    return fl_font_match_faces(faces, count, weight, italic);
}

bool fl_font_file_valid(const char *path)
{
    FT_Library library = NULL;
    if (!path || FT_Init_FreeType(&library)) return false;
    FT_Face face;
    bool ok = !fl_freetype_open(library, path, NULL, 0, 0, &face);
    if (ok) { ok = FT_IS_SCALABLE(face) && face->num_glyphs > 0; FT_Done_Face(face); }
    FT_Done_FreeType(library);
    return ok;
}

#pragma once
#include "core/fl_job.h"
#include "schema/fl_widgets.h"
#include "text/fl_font_catalog.h"

/*
 * Font resolution service: project font assets, then installed system fonts,
 * then the bundled default face. The system catalog is scanned once on a
 * worker; every catalog change bumps the generation and publishes
 * FL_EVENT_FONTS_CHANGED on the owner thread.
 */
typedef struct Fl_Fonts Fl_Fonts;
#define FL_EVENT_FONTS_CHANGED "fonts.changed"
typedef struct Fl_FontsChanged { Fl_Fonts *fonts; uint64_t generation; } Fl_FontsChanged;
typedef enum Fl_FontSource { FL_FONT_SOURCE_PROJECT, FL_FONT_SOURCE_SYSTEM, FL_FONT_SOURCE_BUNDLED } Fl_FontSource;
typedef struct Fl_FontsDesc {
    Fl_EventBus *bus;           /* required; owner thread is the caller */
    Fl_JobSystem *jobs;         /* required when scan_system */
    bool scan_system;           /* enumerate installed fonts in the background */
} Fl_FontsDesc;
/* Resolved face with the synthesis needed to honour the requested style. */
typedef struct Fl_FontMatch {
    const Fl_FontFace *face;    /* never NULL for a live service */
    Fl_FontSource source;
    bool found;                 /* requested family exists in some source */
    bool synthetic_bold, synthetic_italic;
} Fl_FontMatch;

/* Create the service and start the optional system scan. NULL on failure. */
Fl_Fonts *fl_fonts_create(const Fl_FontsDesc *desc);
/* Wait for an in-flight scan, unsubscribe and free every catalog. NULL is safe. */
void fl_fonts_destroy(Fl_Fonts *fonts);
/* Bind (or with NULL, unbind) the project whose font assets take precedence; rebuilds immediately. */
void fl_fonts_set_project(Fl_Fonts *fonts, Fl_Project *project);
/* Resolve typography; faces stay valid until the next generation change. */
Fl_FontMatch fl_fonts_match(const Fl_Fonts *fonts, const Fl_Typography *typography);
/* Report whether the system scan is still running. */
bool fl_fonts_scanning(const Fl_Fonts *fonts);
/* Monotonic counter bumped whenever any catalog changes. */
uint64_t fl_fonts_generation(const Fl_Fonts *fonts);
/* Merged unique family names sorted case-insensitively, with their winning source. */
size_t fl_fonts_family_count(const Fl_Fonts *fonts);
const char *fl_fonts_family(const Fl_Fonts *fonts, size_t index, Fl_FontSource *source);
/* Borrow the winning source's faces for family; zero when the family is unknown. */
size_t fl_fonts_faces(const Fl_Fonts *fonts, const char *family, const Fl_FontFace **faces);

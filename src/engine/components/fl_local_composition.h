#pragma once
#include "fl_prototype_instance.h"

#define FL_LOCAL_COMPOSITION_VERSION 1u
#define FL_LOCAL_COMPOSITION_LIMIT (128u * 1024u * 1024u)
/* Owned native hierarchy and timeline with a parent-to-local clock. No source asset owns this scope. */
typedef struct Fl_LocalComposition {
    Fl_TimeMapping time;
    const char *document, *overrides;
    size_t document_size, overrides_size;
} Fl_LocalComposition;
/* Encode an independently editable native scope; caller owns the resulting component payload. */
bool fl_local_composition_encode(const Fl_LocalComposition *scope, Fl_Component *out);
/* Borrow bounded document/override bytes without transferring their ownership. */
bool fl_local_composition_decode(const Fl_Component *component, Fl_LocalComposition *out);
/* Validate owned scope structure and retained override shapes, independently of project resources. */
bool fl_local_composition_valid(const Fl_Component *component);

#pragma once
#include "doc/fl_json.h"

#define FL_SCRIPT_BINDINGS_LIMIT (16u * 1024u)
#define FL_SCRIPT_BINDING_COUNT 64u

/* Validate named typed entity/component/region/asset references without resolving scene ownership. */
bool fl_script_bindings_valid(Fl_Json *context, const cJSON *bindings);

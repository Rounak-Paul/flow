#pragma once
#include "doc/fl_json.h"

/* Validate exposed definitions and instance values; optionally return an owned effective object with defaults. */
bool fl_parameters_resolve(Fl_Json *context, const cJSON *schema, const cJSON *values, cJSON **effective);

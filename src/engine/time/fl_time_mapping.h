#pragma once
#include "fl_time.h"
#include "doc/fl_binary.h"

typedef struct Fl_Scene Fl_Scene;
typedef enum Fl_RetimeMode { FL_RETIME_STRETCH, FL_RETIME_FIXED } Fl_RetimeMode;
typedef enum Fl_TimeOutside { FL_TIME_INACTIVE, FL_TIME_HOLD } Fl_TimeOutside;
/* A reusable local clock. Disabled explicit_range binds the current scene's whole interval. */
typedef struct Fl_TimeMapping {
    bool explicit_range;
    Fl_TimeAnchor start, end;
    Fl_Tick duration;                 /* canonical local duration; zero uses the region span */
    Fl_RetimeMode mode;
    double rate;
    bool loop;
    Fl_TimeOutside before, after;
} Fl_TimeMapping;
typedef enum Fl_TimePhase { FL_TIME_BEFORE, FL_TIME_ACTIVE, FL_TIME_AFTER } Fl_TimePhase;
/* Resolved clock for one requested composition tick, independent of transport frame rate. */
typedef struct Fl_TimeSample {
    Fl_Tick tick, duration;
    double seconds, progress;
    int64_t cycle;
    Fl_TimePhase phase;
    bool active;
} Fl_TimeSample;
/* Scene-relative stretch, inactive before and held after its final sample. */
Fl_TimeMapping fl_time_mapping_default(void);
/* Validate mapping-local fields without requiring its referenced scene/beat to exist. */
bool fl_time_mapping_valid(const Fl_TimeMapping *mapping);
/* Resolve a local clock; false for an unbound/reversed/zero interval or overflow. */
bool fl_time_mapping_sample(const Fl_Scene *scene, const Fl_TimeMapping *mapping, Fl_Tick tick, Fl_TimeSample *out);
/* Encode/decode internal snapshot fields without native layout assumptions. */
void fl_time_mapping_write(const Fl_TimeMapping *mapping, Fl_Binary *stream);
bool fl_time_mapping_read(Fl_TimeMapping *mapping, Fl_Binary *stream);

/* Invert a local point for deterministic fixed-step input sampling; false for unbound clocks or overflow. */
bool fl_time_mapping_composition(const Fl_Scene *scene, const Fl_TimeMapping *mapping,
                                 Fl_Tick local_tick, int64_t cycle, Fl_Tick *composition_tick);

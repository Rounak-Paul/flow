#pragma once
#include "components/fl_component.h"

#define FL_TICKS_PER_SECOND 120000u

/* Scene time in FL_TICKS_PER_SECOND units. */
typedef int64_t Fl_Tick;
/* Shared point binding: absolute ticks, fixed beat-edge offsets or normalized beat positions. */
typedef enum Fl_AnchorKind { FL_ANCHOR_ABSOLUTE, FL_ANCHOR_BEAT, FL_ANCHOR_BEAT_POSITION } Fl_AnchorKind;
typedef struct Fl_TimeAnchor {
    Fl_AnchorKind kind;
    Fl_Id beat;                 /* stable beat identity for either beat binding kind */
    bool end;                   /* measure from the beat's end edge */
    Fl_Tick offset;             /* ticks after the edge; the scene time itself for FL_ANCHOR_ABSOLUTE */
    double position;           /* FL_ANCHOR_BEAT_POSITION: normalized 0..1, plus fixed offset */
} Fl_TimeAnchor;

/* Convert finite seconds to nearest document tick; rejects values at or outside signed tick limits. */
bool fl_time_from_seconds(double seconds, Fl_Tick *out);

/* Validate an anchor's local structure; referenced beats may be temporarily absent. */
bool fl_time_anchor_valid(const Fl_TimeAnchor *anchor);

/* Encode/decode exact anchor snapshot fields through the shared local-time schema. */
void fl_time_anchor_write(const Fl_TimeAnchor *anchor, Fl_Binary *stream);
bool fl_time_anchor_read(Fl_TimeAnchor *anchor, Fl_Binary *stream);

#pragma once
#include "components/fl_component.h"

#define FL_TICKS_PER_SECOND 120000u

/* Scene time in FL_TICKS_PER_SECOND units. */
typedef int64_t Fl_Tick;
/* Keyframe/clip time: an absolute scene time, or an offset from a beat's start or end edge. */
typedef enum Fl_AnchorKind { FL_ANCHOR_ABSOLUTE, FL_ANCHOR_BEAT } Fl_AnchorKind;
typedef struct Fl_TimeAnchor {
    Fl_AnchorKind kind;
    Fl_Id beat;                 /* FL_ANCHOR_BEAT only */
    bool end;                   /* measure from the beat's end edge */
    Fl_Tick offset;             /* ticks after the edge; the scene time itself for FL_ANCHOR_ABSOLUTE */
} Fl_TimeAnchor;

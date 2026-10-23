#include "fl_time.h"
#include <math.h>
#include <string.h>

bool fl_time_from_seconds(double seconds, Fl_Tick *out)
{
    if (!out || !isfinite(seconds)) return false;
    long double value=(long double)seconds*FL_TICKS_PER_SECOND;
    if (value<=(long double)INT64_MIN || value>=(long double)INT64_MAX) return false;
    *out=(Fl_Tick)llroundl(value); return true;
}

bool fl_time_anchor_valid(const Fl_TimeAnchor *anchor)
{
    if (!anchor) return false;
    if (!isfinite(anchor->position)) return false;
    if (anchor->kind == FL_ANCHOR_SPEECH) return anchor->cue && !anchor->beat && anchor->position == 0;
    if (anchor->cue) return false;
    if (anchor->kind == FL_ANCHOR_ABSOLUTE) return !anchor->beat && !anchor->end && anchor->position == 0;
    if (anchor->kind == FL_ANCHOR_BEAT) return anchor->beat && anchor->position == 0;
    return anchor->kind == FL_ANCHOR_BEAT_POSITION && anchor->beat && !anchor->end &&
        anchor->position >= 0 && anchor->position <= 1;
}

void fl_time_anchor_write(const Fl_TimeAnchor *anchor, Fl_Binary *stream)
{
    fl_binary_u64(stream, anchor->kind); fl_binary_u64(stream, anchor->beat); fl_binary_u64(stream, anchor->end);
    fl_binary_u64(stream, (uint64_t)anchor->offset);
    uint64_t bits; memcpy(&bits, &anchor->position, sizeof(bits)); fl_binary_u64(stream, bits);
    if (anchor->kind == FL_ANCHOR_SPEECH) fl_binary_u64(stream, anchor->cue);
}

bool fl_time_anchor_read(Fl_TimeAnchor *anchor, Fl_Binary *stream)
{
    uint64_t kind = fl_binary_get_u64(stream); anchor->beat = fl_binary_get_u64(stream);
    uint64_t end = fl_binary_get_u64(stream), offset = fl_binary_get_u64(stream), position = fl_binary_get_u64(stream);
    anchor->kind = (Fl_AnchorKind)kind; anchor->end = end != 0;
    memcpy(&anchor->offset, &offset, sizeof(offset)); memcpy(&anchor->position, &position, sizeof(position));
    anchor->cue = kind == FL_ANCHOR_SPEECH ? fl_binary_get_u64(stream) : 0;
    return !stream->failed && kind <= FL_ANCHOR_SPEECH && end <= 1 && fl_time_anchor_valid(anchor);
}

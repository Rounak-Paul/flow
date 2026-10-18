#include "fl_video_clip.h"
#include <math.h>
#include <string.h>

static void put_u64(unsigned char *out, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) out[i] = (unsigned char)(value >> (8 * i));
}

static uint64_t get_u64(const unsigned char *in)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)in[i] << (8 * i);
    return value;
}

void fl_video_clip_encode(const Fl_VideoClip *c, unsigned char out[FL_VIDEO_CLIP_BYTES])
{
    uint64_t rate; memcpy(&rate, &c->rate, 8);
    put_u64(out, c->asset); put_u64(out + 8, (uint64_t)c->start); put_u64(out + 16, (uint64_t)c->offset); put_u64(out + 24, rate);
    out[32] = c->loop ? 1 : 0;
}

bool fl_video_clip_decode(const Fl_Component *c, Fl_VideoClip *out)
{
    if (!c || !out || c->type != FL_COMPONENT_VIDEO || c->version != 1 || c->size != FL_VIDEO_CLIP_BYTES || !c->data) return false;
    Fl_VideoClip v = {.asset = get_u64(c->data), .start = (Fl_Tick)get_u64(c->data + 8), .offset = (Fl_Tick)get_u64(c->data + 16), .loop = c->data[32] == 1};
    uint64_t rate = get_u64(c->data + 24); memcpy(&v.rate, &rate, 8);
    const Fl_Tick limit = (Fl_Tick)FL_TICKS_PER_SECOND * 1000000;
    if (!v.asset || c->data[32] > 1 || v.offset < 0 || v.offset > limit || v.start < -limit || v.start > limit ||
        !isfinite(v.rate) || v.rate <= 0 || v.rate > 16) return false;
    *out = v;
    return true;
}

double fl_video_clip_source_seconds(const Fl_VideoClip *c, Fl_Tick tick, uint64_t duration_ticks)
{
    double elapsed = tick > c->start ? (double)(tick - c->start) * c->rate : 0;
    double ticks = (double)c->offset + elapsed, length = (double)duration_ticks;
    if (length > 0) ticks = c->loop ? fmod(ticks, length) : fmin(ticks, length);
    return ticks / FL_TICKS_PER_SECOND;
}

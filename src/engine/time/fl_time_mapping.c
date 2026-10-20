#include "fl_time_mapping.h"
#include "project/fl_project.h"
#include <math.h>
#include <string.h>

Fl_TimeMapping fl_time_mapping_default(void)
{
    return (Fl_TimeMapping){.rate=1,.after=FL_TIME_HOLD};
}

bool fl_time_mapping_valid(const Fl_TimeMapping *m)
{
    return m && (!m->explicit_range || (fl_time_anchor_valid(&m->start) && fl_time_anchor_valid(&m->end))) && m->duration>=0 &&
        m->mode>=FL_RETIME_STRETCH && m->mode<=FL_RETIME_FIXED && isfinite(m->rate) && m->rate>0 && m->rate<=1000 &&
        m->before>=FL_TIME_INACTIVE && m->before<=FL_TIME_HOLD && m->after>=FL_TIME_INACTIVE && m->after<=FL_TIME_HOLD;
}

bool fl_time_mapping_sample(const Fl_Scene *scene, const Fl_TimeMapping *m, Fl_Tick tick, Fl_TimeSample *out)
{
    if (!scene || !out || !fl_time_mapping_valid(m)) return false;
    Fl_Tick start=0,end=fl_scene_length(scene);
    if (m->explicit_range && (!fl_anchor_sample(scene,&m->start,&start) || !fl_anchor_sample(scene,&m->end,&end))) return false;
    if (end<=start || (start<0 && end>INT64_MAX+start)) return false;
    Fl_Tick span=end-start, canonical=m->duration ? m->duration : span;
    Fl_TimeSample sample={.duration=canonical,.phase=tick<start ? FL_TIME_BEFORE : tick>=end ? FL_TIME_AFTER : FL_TIME_ACTIVE};
    sample.active=sample.phase==FL_TIME_ACTIVE || (sample.phase==FL_TIME_BEFORE ? m->before==FL_TIME_HOLD : m->after==FL_TIME_HOLD);
    long double relative=tick<=start ? 0 : tick>=end ? (long double)span : (long double)tick-(long double)start;
    long double local=relative*(long double)m->rate;
    if (m->mode==FL_RETIME_STRETCH) local=local*(long double)canonical/(long double)span;
    if (!isfinite(local)) return false;
    if (m->loop) {
        long double cycle=floorl(local/(long double)canonical);
        if (cycle>=(long double)INT64_MAX) return false;
        sample.cycle=(int64_t)cycle; local=fmodl(local,(long double)canonical);
        if (sample.phase==FL_TIME_AFTER && local==0 && sample.cycle>0) {local=canonical; --sample.cycle;}
    } else if (local>=(long double)canonical) {
        local=canonical;
        if (m->after==FL_TIME_INACTIVE) sample.active=false;
    }
    if (local<0 || local>(long double)INT64_MAX) return false;
    sample.tick=local>=(long double)INT64_MAX ? INT64_MAX : (Fl_Tick)llroundl(local); sample.seconds=(double)sample.tick/FL_TICKS_PER_SECOND;
    sample.progress=(double)sample.tick/(double)canonical;
    *out=sample; return true;
}

void fl_time_mapping_write(const Fl_TimeMapping *m, Fl_Binary *b)
{
    fl_binary_u64(b,m->explicit_range); fl_time_anchor_write(&m->start,b); fl_time_anchor_write(&m->end,b); fl_binary_u64(b,(uint64_t)m->duration);
    fl_binary_u64(b,m->mode); uint64_t bits; memcpy(&bits,&m->rate,8); fl_binary_u64(b,bits);
    fl_binary_u64(b,m->loop); fl_binary_u64(b,m->before); fl_binary_u64(b,m->after);
}

bool fl_time_mapping_read(Fl_TimeMapping *m, Fl_Binary *b)
{
    uint64_t range=fl_binary_get_u64(b);
    if (!fl_time_anchor_read(&m->start,b) || !fl_time_anchor_read(&m->end,b)) return false;
    uint64_t duration=fl_binary_get_u64(b),mode=fl_binary_get_u64(b),bits=fl_binary_get_u64(b),loop=fl_binary_get_u64(b),before=fl_binary_get_u64(b),after=fl_binary_get_u64(b);
    if (b->failed || range>1 || duration>INT64_MAX || mode>FL_RETIME_FIXED || loop>1 || before>FL_TIME_HOLD || after>FL_TIME_HOLD) return false;
    m->explicit_range=range!=0; m->duration=(Fl_Tick)duration; m->mode=(Fl_RetimeMode)mode; memcpy(&m->rate,&bits,8);
    m->loop=loop!=0; m->before=(Fl_TimeOutside)before; m->after=(Fl_TimeOutside)after;
    return fl_time_mapping_valid(m);
}

bool fl_time_mapping_composition(const Fl_Scene *scene, const Fl_TimeMapping *mapping,
                                 Fl_Tick local_tick, int64_t cycle, Fl_Tick *composition_tick)
{
    if (!scene || !composition_tick || !fl_time_mapping_valid(mapping) || local_tick<0 || cycle<0 || (!mapping->loop && cycle)) return false;
    Fl_Tick start=0,end=fl_scene_length(scene);
    if (mapping->explicit_range && (!fl_anchor_sample(scene,&mapping->start,&start) || !fl_anchor_sample(scene,&mapping->end,&end))) return false;
    if (end<=start || (start<0 && end>INT64_MAX+start)) return false;
    Fl_Tick span=end-start, duration=mapping->duration ? mapping->duration : span;
    if (local_tick>duration) return false;
    long double relative=((long double)cycle*duration+local_tick)/(long double)mapping->rate;
    if (mapping->mode==FL_RETIME_STRETCH) relative=relative*(long double)span/(long double)duration;
    long double result=(long double)start+relative;
    if (!isfinite(result) || result<=(long double)INT64_MIN || result>=(long double)INT64_MAX) return false;
    *composition_tick=(Fl_Tick)llroundl(result); return true;
}

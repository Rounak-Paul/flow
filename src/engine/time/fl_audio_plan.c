#include "fl_audio_plan.h"
#include "fl_composition.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define AUDIO_INTERVALS 4096u
#define AUDIO_SCOPES 4096u
#define AUDIO_RATE_MIN (1.0/65536.0)
#define AUDIO_RATE_MAX 65536.0

typedef struct Interval {long double start,end,local,rate;} Interval;
typedef struct Clock {const Fl_Scene *scene; Interval *intervals; size_t count,capacity;} Clock;

/* Bounded compilation failure leaves no partial playback schedule. */
static bool fail(Fl_AudioPlan *plan, const char *message)
{
    if (!plan->error[0]) snprintf(plan->error,sizeof(plan->error),"%s",message); return false;
}

/* Append an active affine clock interval, rejecting inaudible/unrepresentable rates and expansion explosions. */
static bool interval(Fl_AudioPlan *plan, Clock *clock, Interval value, size_t *budget)
{
    if (value.end<=value.start) return true;
    if (!isfinite(value.start) || !isfinite(value.end) || !isfinite(value.local) || !isfinite(value.rate) ||
        value.rate<AUDIO_RATE_MIN || value.rate>AUDIO_RATE_MAX) return fail(plan,"Audio clock rate is outside 1/65536..65536");
    if (*budget==AUDIO_INTERVALS) return fail(plan,"Audio clock expansion exceeds 4096 intervals");
    if (clock->count==clock->capacity) {
        size_t capacity=clock->capacity ? clock->capacity*2 : 8;
        Interval *grown=realloc(clock->intervals,capacity*sizeof(*grown));
        if (!grown) return fail(plan,"Cannot allocate audio clock intervals");
        clock->intervals=grown; clock->capacity=capacity;
    }
    clock->intervals[clock->count++]=value; ++*budget; return true;
}

/* Compose a child's region with each active parent interval, splitting precisely at loop boundaries. */
static bool map_clock(Fl_AudioPlan *plan, Clock *clock, const Clock *parent, const Fl_TimeMapping *mapping, size_t *budget)
{
    Fl_Tick start=0,end=fl_scene_length(parent->scene);
    if (mapping->explicit_range && (!fl_anchor_sample(parent->scene,&mapping->start,&start) || !fl_anchor_sample(parent->scene,&mapping->end,&end))) return true;
    if (end<=start) return true;
    long double span=(long double)end-start,canonical=mapping->duration ? mapping->duration : span;
    long double speed=mapping->rate*(mapping->mode==FL_RETIME_STRETCH ? canonical/span : 1);
    for (size_t i=0;i<parent->count;++i) {
        const Interval *p=&parent->intervals[i];
        long double left=fmaxl(p->local,start),right=fminl(p->local+(p->end-p->start)*p->rate,end);
        if (!mapping->loop) right=fminl(right,(long double)start+canonical/speed);
        while (left<right) {
            long double raw=(left-start)*speed,cycle=mapping->loop ? floorl(raw/canonical) : 0;
            long double stop=mapping->loop ? fminl(right,(long double)start+(cycle+1)*canonical/speed) : right;
            if (stop<=left) return fail(plan,"Audio loop precision exhausted");
            Interval child={.start=p->start+(left-p->local)/p->rate,.end=p->start+(stop-p->local)/p->rate,
                .local=fmaxl(0,raw-cycle*canonical),.rate=p->rate*speed};
            if (!interval(plan,clock,child,budget)) return false;
            left=stop;
        }
    }
    return true;
}

/* Resolve clip source ranges and fades without changing authored regions or prototype contents. */
static bool emit(Fl_AudioPlan *plan, const Clock *clock)
{
    for (size_t i=0;i<clock->scene->clip_count;++i) {
        const Fl_AudioClip *clip=&clock->scene->clips[i]; Fl_Tick start;
        const Fl_Asset *asset=fl_project_asset(clock->scene->project,clip->asset);
        if (!asset || asset->kind!=FL_ASSET_AUDIO) return fail(plan,"Missing audio resource");
        if (!fl_anchor_sample(clock->scene,&clip->start,&start)) continue;
        long double available=asset->duration>clip->source_offset ? asset->duration-clip->source_offset : 0;
        if (!asset->duration && !clip->duration) return fail(plan,"Audio resource has no duration; reimport it or set an explicit clip duration");
        long double natural=clip->duration ? clip->duration : available;
        long double audible=asset->duration ? fminl(natural,available) : natural;
        if (!audible) continue;
        for (size_t j=0;j<clock->count;++j) {
            const Interval *p=&clock->intervals[j];
            long double left=fmaxl(start,p->local),right=fminl((long double)start+audible,p->local+(p->end-p->start)*p->rate);
            if (right<=left) continue;
            if (plan->count==AUDIO_INTERVALS) return fail(plan,"Audio clip expansion exceeds 4096 segments");
            if (plan->count==plan->capacity) {
                size_t capacity=plan->capacity ? plan->capacity*2 : 8;
                Fl_AudioSegment *grown=realloc(plan->segments,capacity*sizeof(*grown));
                if (!grown) return fail(plan,"Cannot allocate audio segments");
                plan->segments=grown; plan->capacity=capacity;
            }
            plan->segments[plan->count++]=(Fl_AudioSegment){.asset=clip->asset,.start=(double)((p->start+(left-p->local)/p->rate)/FL_TICKS_PER_SECOND),
                .duration=(double)((right-left)/p->rate/FL_TICKS_PER_SECOND),.source_offset=(double)(((long double)clip->source_offset+left-start)/FL_TICKS_PER_SECOND),
                .rate=(double)p->rate,.gain=clip->gain,.envelope_offset=(double)((left-start)/FL_TICKS_PER_SECOND),
                .envelope_duration=(double)(natural/FL_TICKS_PER_SECOND),.fade_in=(double)clip->fade_in/FL_TICKS_PER_SECOND,.fade_out=(double)clip->fade_out/FL_TICKS_PER_SECOND};
        }
    }
    return true;
}

bool fl_audio_plan_compile(const Fl_Scene *scene, Fl_AudioPlan *out)
{
    if (!scene || !out || out->segments || out->count) return false;
    Fl_Composition *composition=fl_composition_required(scene) ? fl_composition_create(scene,NULL,out->error,sizeof(out->error)) : NULL;
    if (fl_composition_required(scene) && !composition) return false;
    Clock *clocks=calloc(AUDIO_SCOPES,sizeof(*clocks)); bool ok=clocks!=NULL; size_t count=0,budget=0;
    if (!ok) fail(out,"Cannot allocate audio clocks");
    for (size_t i=0;ok && i<AUDIO_SCOPES;++i) {
        const Fl_Scene *local; const Fl_TimeMapping *mapping=NULL; size_t parent=SIZE_MAX;
        if (composition) {if (!fl_composition_scope(composition,i,&local,&parent,&mapping)) break;}
        else {if (i) break; local=scene;}
        Clock *clock=&clocks[i]; clock->scene=local; ++count;
        if (!i) ok=interval(out,clock,(Interval){.end=fl_scene_length(scene),.rate=1},&budget);
        else if (parent>=i) ok=fail(out,"Invalid audio scope dependency");
        else ok=map_clock(out,clock,&clocks[parent],mapping,&budget);
        if (ok) ok=emit(out,clock);
    }
    for (size_t i=0;i<count;++i) free(clocks[i].intervals);
    free(clocks); fl_composition_destroy(composition);
    if (!ok) {free(out->segments); out->segments=NULL; out->count=0; out->capacity=0;} return ok;
}
void fl_audio_plan_clear(Fl_AudioPlan *plan)
{
    if (!plan) return; free(plan->segments); memset(plan,0,sizeof(*plan));
}

/* Reject overflow before converting public double seconds back to exact native ticks. */
static bool ticks_of(double seconds, Fl_Tick *out)
{
    long double ticks=(long double)seconds*FL_TICKS_PER_SECOND;
    if (!isfinite(ticks) || ticks<0 || ticks>=(long double)INT64_MAX) return false;
    *out=(Fl_Tick)llroundl(ticks); return true;
}

bool fl_audio_segment_clip(const Fl_Project *project, const Fl_AudioSegment *segment, double origin, Fl_MixerClip *out)
{
    if (!project || !segment || !out || !isfinite(origin) || origin<0) return false;
    const Fl_Asset *asset=fl_project_asset(project,segment->asset); *out=(Fl_MixerClip){0};
    if (!asset || !fl_project_asset_path(project,asset,out->path,sizeof(out->path)) ||
        !ticks_of(segment->start+origin,&out->start) || !ticks_of(segment->duration,&out->duration) ||
        !ticks_of(segment->source_offset,&out->source_offset) || !ticks_of(segment->envelope_offset,&out->envelope_offset) ||
        !ticks_of(segment->envelope_duration,&out->envelope_duration) || !ticks_of(segment->fade_in,&out->fade_in) || !ticks_of(segment->fade_out,&out->fade_out)) return false;
    if (!out->duration) out->duration=1;
    out->rate=segment->rate; out->gain=segment->gain; return true;
}

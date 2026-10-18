#include "fl_animation_player.h"
#include "fl_scene_state.h"
#include <math.h>
#include <string.h>

void fl_animation_player_reset(Fl_AnimationPlayer *p,const Fl_Animation *a)
{
    *p=(Fl_AnimationPlayer){.state=a ? a->entry : 0,.playing=true};
    for (size_t i=0;a&&i<a->parameter_count;++i) p->parameters[i]=a->parameters[i].value;
}

bool fl_animation_player_play(Fl_AnimationPlayer *p,const Fl_Animation *a,Fl_Id state,uint64_t blend)
{
    if (!p||!fl_animation_state(a,state)||blend>(UINT64_C(1)<<62)) return false;
    if (p->state==state) {p->playing=true;return true;}
    p->previous=p->state;p->previous_elapsed=p->elapsed;p->state=state;p->elapsed=0;
    p->blend_elapsed=0;p->blend_duration=blend;p->blending=blend!=0;p->playing=true;return true;
}

/* Advance bounded signed time without wrapping. */
static Fl_Tick advance(Fl_Tick time,Fl_Tick step) {return step>INT64_MAX-time ? INT64_MAX : time+step;}

void fl_animation_player_step(Fl_AnimationPlayer *p,const Fl_Animation *a,const Fl_Scene *scene,Fl_Tick step)
{
    if (!p||!a||!p->playing||step<0) return;
    p->elapsed=advance(p->elapsed,step);
    if (p->blending) {
        p->previous_elapsed=advance(p->previous_elapsed,step);p->blend_elapsed=advance(p->blend_elapsed,step);
        if ((uint64_t)p->blend_elapsed>=p->blend_duration) p->blending=false;
    }
    const Fl_AnimationState *state=fl_animation_state(a,p->state);
    uint64_t duration=p->state&&state ? state->duration : (uint64_t)fl_scene_length(scene);
    for (size_t i=0;i<a->transition_count;++i) {
        const Fl_AnimationTransition *edge=&a->transitions[i];
        if (edge->from!=p->state||(edge->at_end&&(uint64_t)p->elapsed<duration)) continue;
        size_t parameter=0;
        while (parameter<a->parameter_count&&a->parameters[parameter].id!=edge->parameter) ++parameter;
        double value=parameter<a->parameter_count ? p->parameters[parameter] : 0;
        bool matches=edge->condition==FL_ANIM_ALWAYS || (edge->condition==FL_ANIM_GREATER&&value>edge->threshold) ||
            (edge->condition==FL_ANIM_LESS&&value<edge->threshold)||(edge->condition==FL_ANIM_EQUAL&&value==edge->threshold)||
            (edge->condition==FL_ANIM_FIRED&&value!=0);
        if (!matches) continue;
        if (fl_animation_player_play(p,a,edge->to,edge->blend_duration)&&edge->condition==FL_ANIM_FIRED&&parameter<a->parameter_count) p->parameters[parameter]=0;
        break;
    }
}

/* Extract authored bind channels, used as the neutral animation pose. */
static void bind_values(const Fl_Entity *e,double values[FL_PROP_COUNT][4])
{
    Fl_Transform t={.scale_x=1,.scale_y=1};fl_transform_decode(fl_entity_component(e,FL_COMPONENT_TRANSFORM),&t);
    Fl_Appearance appearance=fl_widget_appearance(e);
    const double scalar[]={t.x,t.y,t.width,t.height,t.rotation,t.scale_x,t.scale_y,fl_widget_opacity(e),0,0,appearance.stroke_width,appearance.font_size};
    for (int p=0;p<FL_PROP_COUNT;++p) values[p][0]=scalar[p];
    for (int c=0;c<4;++c) {values[FL_PROP_FILL][c]=appearance.fill[c];values[FL_PROP_STROKE][c]=appearance.stroke[c];}
}

/* Resolve state-local time and sample its tracks into an absolute authored pose. */
static void pose(const Fl_AnimationState *state,const Fl_Scene *scene,Fl_Tick elapsed,Fl_Tick tick,double values[FL_PROP_COUNT][4])
{
    if (!state) return;
    Fl_Tick at=tick;
    if (state->id) at=state->loop ? elapsed%(Fl_Tick)state->duration : elapsed>(Fl_Tick)state->duration ? (Fl_Tick)state->duration : elapsed;
    for (size_t i=0;i<state->track_count;++i) fl_track_sample(scene,&state->tracks[i],at,values[state->tracks[i].property]);
}

void fl_animation_player_pose(const Fl_AnimationPlayer *p,const Fl_Animation *a,const Fl_Scene *scene,const Fl_Entity *entity,Fl_Tick tick,Fl_EntityState *output)
{
    if (!a||!output) return;
    double bind[FL_PROP_COUNT][4]={{0}},current[FL_PROP_COUNT][4],previous[FL_PROP_COUNT][4];
    bind_values(entity,bind);memcpy(current,bind,sizeof(bind));memcpy(previous,bind,sizeof(bind));
    pose(fl_animation_state(a,p ? p->state : 0),scene,p ? p->elapsed : tick,p ? p->elapsed : tick,current);
    double weight=1;
    if (p&&p->blending) {
        pose(fl_animation_state(a,p->previous),scene,p->previous_elapsed,p->previous_elapsed,previous);
        weight=p->blend_duration ? fmin(1,(double)p->blend_elapsed/p->blend_duration) : 1;
    }
    double result[FL_PROP_COUNT][4]={{0}};
    Fl_EntityState composed=*output;
    const double scalar[]={output->transform.x,output->transform.y,output->transform.width,output->transform.height,output->transform.rotation,
        output->transform.scale_x,output->transform.scale_y,output->opacity,0,0,output->appearance.stroke_width,output->appearance.font_size};
    for (int property=0;property<FL_PROP_COUNT;++property) {
        bool colour=property==FL_PROP_FILL||property==FL_PROP_STROKE;
        for (int c=0;c<(colour ? 4 : 1);++c) {
            double animated=previous[property][c]+(current[property][c]-previous[property][c])*weight;
            double base=colour ? (property==FL_PROP_FILL ? output->appearance.fill[c] : output->appearance.stroke[c]) : scalar[property];
            if (base==bind[property][c]) result[property][c]=animated;
            else if ((property==FL_PROP_SCALE_X||property==FL_PROP_SCALE_Y||property==FL_PROP_OPACITY)&&bind[property][c]!=0)
                result[property][c]=(double)((long double)base*animated/bind[property][c]);
            else result[property][c]=base+(animated-bind[property][c]);
            if (!isfinite(result[property][c])) return;
        }
        fl_entity_state_apply(&composed,(Fl_AnimProperty)property,result[property]);
    }
    *output=composed;
}

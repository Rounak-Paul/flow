#pragma once
#include "components/fl_animation.h"

typedef struct Fl_Scene Fl_Scene;
typedef struct Fl_EntityState Fl_EntityState;
/* Runtime state and graph parameters; document definitions remain immutable. */
typedef struct Fl_AnimationPlayer {
    Fl_Id state, previous;
    Fl_Tick elapsed, previous_elapsed, blend_elapsed;
    uint64_t blend_duration;
    bool blending, playing;
    double parameters[FL_ANIMATION_GRAPH_LIMIT];
} Fl_AnimationPlayer;

/* Reset player to the graph entry state and default parameter values. */
void fl_animation_player_reset(Fl_AnimationPlayer *player,const Fl_Animation *animation);
/* Play state, preserving an already active state; blend from the prior state when requested. */
bool fl_animation_player_play(Fl_AnimationPlayer *player,const Fl_Animation *animation,Fl_Id state,uint64_t blend);
/* Advance one deterministic step and evaluate the first matching graph edge. */
void fl_animation_player_step(Fl_AnimationPlayer *player,const Fl_Animation *animation,const Fl_Scene *scene,Fl_Tick step);
/* Compose the sampled/blended animation pose with procedural base values. NULL player selects Timeline. */
void fl_animation_player_pose(const Fl_AnimationPlayer *player,const Fl_Animation *animation,const Fl_Scene *scene,
                              const Fl_Entity *entity,Fl_Tick tick,Fl_EntityState *state);

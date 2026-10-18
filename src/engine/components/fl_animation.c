#include "fl_animation.h"
#include "project/fl_project.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const Fl_Animation *fl_entity_animation(const Fl_Entity *e)
{
    const Fl_Component *c = fl_entity_component(e, FL_COMPONENT_ANIMATION);
    return c && c->version == 2 && c->size == sizeof(Fl_Animation) ? (const Fl_Animation *)c->data : NULL;
}

Fl_Animation *fl_animation_require(Fl_Entity *e)
{
    Fl_Animation *a = (Fl_Animation *)fl_entity_animation(e);
    if (a || !e || e->component_count >= SIZE_MAX / sizeof(Fl_Component) - 1) return a;
    a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    Fl_Component *components = realloc((void *)e->components, (e->component_count + 1) * sizeof(*components));
    if (!components) {free(a); return NULL;}
    e->components = components;
    components[e->component_count++] = (Fl_Component){FL_COMPONENT_ANIMATION, 2, sizeof(*a), (unsigned char *)a};
    return a;
}

void fl_animation_destroy(Fl_Animation *a)
{
    if (!a) return;
    for (size_t i = 0; i < a->timeline.track_count; ++i) free(a->timeline.tracks[i].keys);
    for (size_t i = 0; i < a->state_count; ++i) {
        for (size_t t = 0; t < a->states[i].track_count; ++t) free(a->states[i].tracks[t].keys);
        free((void *)a->states[i].name);
    }
    for (size_t i = 0; i < a->parameter_count; ++i) free((void *)a->parameters[i].name);
    free(a->states); free(a->parameters); free(a->transitions); free(a);
}

Fl_Animation *fl_animation_clone(const Fl_Animation *a)
{
    if (!a) return NULL;
    Fl_Binary bytes = {0}; fl_animation_write(a, &bytes);
    Fl_Animation *copy = bytes.failed ? NULL : fl_animation_read(&bytes, 2);
    free(bytes.data);
    if (copy) {
        for (size_t i=0;i<copy->timeline.track_count;++i) copy->timeline.tracks[i].entity=a->timeline.tracks[i].entity;
        for (size_t i=0;i<copy->state_count;++i) for (size_t t=0;t<copy->states[i].track_count;++t) copy->states[i].tracks[t].entity=a->states[i].tracks[t].entity;
    }
    return copy;
}

bool fl_animation_valid(const Fl_Animation *a)
{
    if (!a || a->timeline.track_count > FL_PROP_COUNT) return false;
    uint32_t properties = 0;
    for (size_t i = 0; i < a->timeline.track_count; ++i) {
        const Fl_Track *t = &a->timeline.tracks[i];
        if (!t->id || !t->entity || t->property < 0 || t->property >= FL_PROP_COUNT ||
            !t->key_count || t->key_count > t->key_capacity || !t->keys || (properties & (1u << t->property))) return false;
        properties |= 1u << t->property;
        for (size_t k = 0; k < t->key_count; ++k)
            if (!t->keys[k].id || t->keys[k].ease < 0 || t->keys[k].ease >= FL_EASE_COUNT ||
                !fl_property_value_valid(t->property, t->keys[k].value)) return false;
    }
    if (a->state_count>FL_ANIMATION_GRAPH_LIMIT||a->parameter_count>FL_ANIMATION_GRAPH_LIMIT||a->transition_count>FL_ANIMATION_GRAPH_LIMIT||!fl_animation_state(a,a->entry)) return false;
    for (size_t i=0;i<a->state_count;++i) {
        const Fl_AnimationState *state=&a->states[i];
        if (!state->id||!state->name||!state->name[0]||strlen(state->name)>255||!strcmp(state->name,"Timeline")||!state->duration||state->duration>(UINT64_C(1)<<62)||state->track_count>FL_PROP_COUNT) return false;
        for (size_t j=0;j<i;++j) if (a->states[j].id==state->id||!strcmp(a->states[j].name,state->name)) return false;
        uint32_t owned=0;
        for (size_t t=0;t<state->track_count;++t) {
            const Fl_Track *track=&state->tracks[t];
            if (!track->id||track->property<0||track->property>=FL_PROP_COUNT||!track->key_count||track->key_count>track->key_capacity||!track->keys||(owned&(1u<<track->property))) return false;
            owned|=1u<<track->property;
            for (size_t k=0;k<track->key_count;++k) {
                const Fl_Keyframe *key=&track->keys[k];
                if (!key->id||key->at.kind!=FL_ANCHOR_ABSOLUTE||key->at.beat||key->at.end||key->at.offset<0||(uint64_t)key->at.offset>state->duration||key->ease<0||key->ease>=FL_EASE_COUNT||!fl_property_value_valid(track->property,key->value)) return false;
            }
        }
    }
    for (size_t i=0;i<a->parameter_count;++i) {
        const Fl_AnimationParameter *parameter=&a->parameters[i];
        if (!parameter->id||!parameter->name||!parameter->name[0]||strlen(parameter->name)>255||parameter->kind<0||parameter->kind>FL_ANIM_TRIGGER||!isfinite(parameter->value)||(parameter->kind!=FL_ANIM_NUMBER&&parameter->value!=0&&parameter->value!=1)) return false;
        for (size_t j=0;j<i;++j) if (a->parameters[j].id==parameter->id||!strcmp(a->parameters[j].name,parameter->name)) return false;
    }
    for (size_t i=0;i<a->transition_count;++i) {
        const Fl_AnimationTransition *edge=&a->transitions[i];
        if (!edge->id||edge->from==edge->to||!fl_animation_state(a,edge->from)||!fl_animation_state(a,edge->to)||edge->condition<0||edge->condition>FL_ANIM_FIRED||!isfinite(edge->threshold)||edge->blend_duration>(UINT64_C(1)<<62)) return false;
        const Fl_AnimationParameter *parameter=NULL;
        for (size_t p=0;p<a->parameter_count;++p) if (a->parameters[p].id==edge->parameter) parameter=&a->parameters[p];
        if (edge->condition==FL_ANIM_ALWAYS) {if (edge->parameter) return false;}
        else if (!parameter||(edge->condition==FL_ANIM_FIRED ? parameter->kind!=FL_ANIM_TRIGGER : parameter->kind==FL_ANIM_TRIGGER)) return false;
    }
    return true;
}

/* Encode/decode binary64 values independently of native structure layout. */
static void put_double(Fl_Binary *b, double v) {uint64_t bits; memcpy(&bits, &v, 8); fl_binary_u64(b, bits);}
static double get_double(Fl_Binary *b) {uint64_t bits = fl_binary_get_u64(b); double v; memcpy(&v, &bits, 8); return v;}

static void write_state_tracks(const Fl_AnimationState *a, Fl_Binary *b)
{
    fl_binary_u64(b, a->track_count);
    for (size_t i = 0; i < a->track_count; ++i) {
        const Fl_Track *t = &a->tracks[i];
        fl_binary_u64(b, t->id); fl_binary_u64(b, t->property); fl_binary_u64(b, t->key_count);
        for (size_t k = 0; k < t->key_count; ++k) {
            const Fl_Keyframe *key = &t->keys[k];
            fl_binary_u64(b, key->id); fl_binary_u64(b, key->at.kind); fl_binary_u64(b, key->at.beat);
            fl_binary_u64(b, key->at.end); fl_binary_u64(b, (uint64_t)key->at.offset);
            for (int c = 0; c < 4; ++c) put_double(b, key->value[c]);
            fl_binary_u64(b, key->ease);
        }
    }
}

static bool read_state_tracks(Fl_AnimationState *a, Fl_Binary *b)
{
    uint64_t count = fl_binary_get_u64(b);
    if (b->failed || count > FL_PROP_COUNT) return false;
    for (size_t i = 0; i < count && !b->failed; ++i) {
        Fl_Track *t = &a->tracks[a->track_count++];
        t->id = fl_binary_get_u64(b);
        uint64_t property = fl_binary_get_u64(b), keys = fl_binary_get_u64(b);
        if (b->failed || property >= FL_PROP_COUNT || keys > SIZE_MAX / sizeof(Fl_Keyframe) || keys > (b->size - b->cursor) / 80) {b->failed = true; break;}
        t->property = (Fl_AnimProperty)property;
        t->keys = keys ? calloc((size_t)keys, sizeof(*t->keys)) : NULL;
        if (keys && !t->keys) {b->failed = true; break;}
        t->key_count = t->key_capacity = (size_t)keys;
        for (size_t k = 0; k < keys && !b->failed; ++k) {
            Fl_Keyframe *key = &t->keys[k];
            key->id = fl_binary_get_u64(b);
            uint64_t kind = fl_binary_get_u64(b); key->at.beat = fl_binary_get_u64(b);
            uint64_t end = fl_binary_get_u64(b), offset = fl_binary_get_u64(b);
            memcpy(&key->at.offset, &offset, sizeof(offset));
            key->at.kind = (Fl_AnchorKind)kind; key->at.end = end != 0;
            for (int c = 0; c < 4; ++c) key->value[c] = get_double(b);
            uint64_t ease = fl_binary_get_u64(b); key->ease = (Fl_Ease)ease;
            if (kind > FL_ANCHOR_BEAT || end > 1 || ease >= FL_EASE_COUNT) b->failed = true;
        }
    }
    return !b->failed;
}

bool fl_property_value_valid(Fl_AnimProperty property, const double v[4])
{
    if (!v) return false;
    for (int i = 0; i < 4; ++i) if (!isfinite(v[i])) return false;
    switch (property) {
    case FL_PROP_X: case FL_PROP_Y: case FL_PROP_ROTATION: case FL_PROP_SCALE_X: case FL_PROP_SCALE_Y:
        return fabs(v[0]) <= 1e9;
    case FL_PROP_WIDTH: case FL_PROP_HEIGHT: return v[0] >= 0 && v[0] <= 1e9;
    case FL_PROP_OPACITY: return v[0] >= 0 && v[0] <= 1;
    case FL_PROP_FILL: case FL_PROP_STROKE:
        for (int i = 0; i < 4; ++i) if (v[i] < 0 || v[i] > 255) return false;
        return true;
    case FL_PROP_STROKE_WIDTH: return v[0] >= 0 && v[0] <= 65536;
    case FL_PROP_FONT_SIZE: return v[0] >= 1 && v[0] <= 2048;
    default: return false;
    }
}


const Fl_AnimationState *fl_animation_state(const Fl_Animation *a, Fl_Id id)
{
    if (!a) return NULL;
    if (!id) return &a->timeline;
    for (size_t i=0;i<a->state_count;++i) if (a->states[i].id==id) return &a->states[i];
    return NULL;
}

const Fl_AnimationState *fl_animation_state_named(const Fl_Animation *a,const char *name)
{
    if (!a||!name) return NULL;
    if (!strcmp(name,"Timeline")) return &a->timeline;
    for (size_t i=0;i<a->state_count;++i) if (!strcmp(a->states[i].name,name)) return &a->states[i];
    return NULL;
}

/* Own a bounded graph name independent of caller storage. */
static char *graph_name(const char *name)
{
    if (!name||!name[0]||strlen(name)>255) return NULL;
    char *copy=malloc(strlen(name)+1);if (copy) strcpy(copy,name);return copy;
}

/* Read a bounded graph collection count before allocation. */
static size_t graph_count(Fl_Binary *b,size_t minimum)
{
    uint64_t count=fl_binary_get_u64(b);
    if (b->failed||count>FL_ANIMATION_GRAPH_LIMIT||count>(b->size-b->cursor)/minimum) {b->failed=true;return 0;}
    return (size_t)count;
}

void fl_animation_write(const Fl_Animation *a,Fl_Binary *b)
{
    write_state_tracks(&a->timeline,b);
    fl_binary_u64(b,a->entry);fl_binary_u64(b,a->state_count);
    for (size_t i=0;i<a->state_count;++i) {
        const Fl_AnimationState *state=&a->states[i];
        fl_binary_u64(b,state->id);fl_binary_string(b,state->name);fl_binary_u64(b,state->duration);fl_binary_u64(b,state->loop);
        write_state_tracks(state,b);
    }
    fl_binary_u64(b,a->parameter_count);
    for (size_t i=0;i<a->parameter_count;++i) {
        const Fl_AnimationParameter *p=&a->parameters[i];
        fl_binary_u64(b,p->id);fl_binary_string(b,p->name);fl_binary_u64(b,p->kind);put_double(b,p->value);
    }
    fl_binary_u64(b,a->transition_count);
    for (size_t i=0;i<a->transition_count;++i) {
        const Fl_AnimationTransition *t=&a->transitions[i];
        fl_binary_u64(b,t->id);fl_binary_u64(b,t->from);fl_binary_u64(b,t->to);fl_binary_u64(b,t->parameter);
        fl_binary_u64(b,t->condition);put_double(b,t->threshold);fl_binary_u64(b,t->blend_duration);fl_binary_u64(b,t->at_end);
    }
}

Fl_Animation *fl_animation_read(Fl_Binary *b,uint64_t version)
{
    if (version!=1&&version!=2) return NULL;
    Fl_Animation *a=calloc(1,sizeof(*a));if (!a) return NULL;
    if (!read_state_tracks(&a->timeline,b)) goto failed;
    if (version==1) return a;
    a->entry=fl_binary_get_u64(b);
    size_t count=graph_count(b,40);
    a->states=count ? calloc(count,sizeof(*a->states)) : NULL;
    if (count&&!a->states) goto failed;
    for (size_t i=0;i<count&&!b->failed;++i) {
        Fl_AnimationState *state=&a->states[a->state_count++];
        state->id=fl_binary_get_u64(b);state->name=fl_binary_get_string(b);state->duration=fl_binary_get_u64(b);
        uint64_t loop=fl_binary_get_u64(b);state->loop=loop!=0;
        if (loop>1||!state->name||!read_state_tracks(state,b)) goto failed;
    }
    count=graph_count(b,32);a->parameters=count ? calloc(count,sizeof(*a->parameters)) : NULL;
    if (count&&!a->parameters) goto failed;
    for (size_t i=0;i<count&&!b->failed;++i) {
        Fl_AnimationParameter *p=&a->parameters[a->parameter_count++];
        p->id=fl_binary_get_u64(b);p->name=fl_binary_get_string(b);
        uint64_t kind=fl_binary_get_u64(b);p->kind=(Fl_AnimationParameterKind)kind;p->value=get_double(b);
        if (kind>FL_ANIM_TRIGGER||!p->name) goto failed;
    }
    count=graph_count(b,64);a->transitions=count ? calloc(count,sizeof(*a->transitions)) : NULL;
    if (count&&!a->transitions) goto failed;
    for (size_t i=0;i<count&&!b->failed;++i) {
        Fl_AnimationTransition *t=&a->transitions[a->transition_count++];
        t->id=fl_binary_get_u64(b);t->from=fl_binary_get_u64(b);t->to=fl_binary_get_u64(b);t->parameter=fl_binary_get_u64(b);
        uint64_t condition=fl_binary_get_u64(b);t->condition=(Fl_AnimationCondition)condition;t->threshold=get_double(b);
        t->blend_duration=fl_binary_get_u64(b);uint64_t end=fl_binary_get_u64(b);t->at_end=end!=0;
        if (condition>FL_ANIM_FIRED||end>1) goto failed;
    }
    if (!b->failed) return a;
failed:
    fl_animation_destroy(a);return NULL;
}

/* Release one state and compact its graph edges without retaining dangling references. */
static void remove_state(Fl_Animation *a,size_t index)
{
    Fl_AnimationState *s=&a->states[index];
    for (size_t i=0;i<s->track_count;++i) free(s->tracks[i].keys);
    free((void *)s->name);
    size_t kept=0;for (size_t i=0;i<a->transition_count;++i)
        if (a->transitions[i].from!=s->id&&a->transitions[i].to!=s->id) a->transitions[kept++]=a->transitions[i];
    a->transition_count=kept;if (a->entry==s->id) a->entry=0;
    memmove(s,s+1,(a->state_count-index-1)*sizeof(*s));--a->state_count;
}

/* Edit a state-local property track in a private draft. */
static bool state_key(Fl_AnimationState *s,const Fl_Op *op,uint64_t *next,Fl_Id *created)
{
    if (!s||!s->id) return false;
    Fl_Track *track=NULL;
    for (size_t t=0;t<s->track_count;++t) if (s->tracks[t].property==op->property) track=&s->tracks[t];
    if (op->kind==FL_OP_KEY_REMOVE||op->kind==FL_OP_TRACK_REMOVE) {
        for (size_t t=0;t<s->track_count;++t) {
            Fl_Track *current=&s->tracks[t];
            for (size_t k=0;k<current->key_count;++k) if ((op->kind==FL_OP_TRACK_REMOVE&&current->id==op->track)||current->keys[k].id==op->key) {
                if (op->kind==FL_OP_TRACK_REMOVE) current->key_count=0;
                else {memmove(current->keys+k,current->keys+k+1,(current->key_count-k-1)*sizeof(*current->keys));--current->key_count;}
                if (!current->key_count) {free(current->keys);memmove(current,current+1,(s->track_count-t-1)*sizeof(*current));--s->track_count;}
                return true;
            }
        }
        return false;
    }
    if (op->anchor.kind!=FL_ANCHOR_ABSOLUTE||op->anchor.beat||op->anchor.end||op->anchor.offset<0||
        (uint64_t)op->anchor.offset>s->duration||op->ease<0||op->ease>=FL_EASE_COUNT||!fl_property_value_valid(op->property,op->value)) return false;
    Fl_Keyframe key={.at=op->anchor,.ease=op->ease};memcpy(key.value,op->value,sizeof(key.value));
    for (size_t k=0;track&&k<track->key_count;++k) if (op->key ? track->keys[k].id==op->key : track->keys[k].at.offset==op->anchor.offset) {
        key.id=track->keys[k].id;track->keys[k]=key;*created=key.id;return true;
    }
    if (op->key||*next>=UINT64_MAX-1) return false;
    if (!track) {
        if (s->track_count==FL_PROP_COUNT) return false;
        track=&s->tracks[s->track_count++];*track=(Fl_Track){.id=(*next)++,.entity=op->entity,.property=op->property};
    }
    if (track->key_count>=SIZE_MAX/sizeof(*track->keys)-1) return false;
    Fl_Keyframe *keys=realloc(track->keys,(track->key_count+1)*sizeof(*keys));if (!keys) return false;
    track->keys=keys;track->key_capacity=track->key_count+1;key.id=(*next)++;keys[track->key_count++]=key;*created=key.id;return true;
}

bool fl_animation_apply(Fl_Animation *a,const Fl_Op *op,uint64_t *next,Fl_Id *created)
{
    if (!a||!op||!next||!created) return false;
    if (op->kind==FL_OP_KEY_SET||op->kind==FL_OP_KEY_REMOVE||op->kind==FL_OP_TRACK_REMOVE)
        return state_key((Fl_AnimationState *)fl_animation_state(a,op->animation_state),op,next,created);
    switch (op->kind) {
    case FL_OP_ANIMATION_STATE_SET: {
        const Fl_AnimationState *input=op->animation_state_record;
        if (!input||!input->duration||input->duration>(UINT64_C(1)<<62)||input->track_count||!input->name||!strcmp(input->name,"Timeline")) return false;
        for (size_t i=0;i<a->state_count;++i) if (a->states[i].id!=op->animation_state&&!strcmp(a->states[i].name,input->name)) return false;
        Fl_AnimationState *state=op->animation_state ? (Fl_AnimationState *)fl_animation_state(a,op->animation_state) : NULL;
        if (op->animation_state&&!state) return false;
        for (size_t t=0;state&&t<state->track_count;++t) for (size_t k=0;k<state->tracks[t].key_count;++k)
            if ((uint64_t)state->tracks[t].keys[k].at.offset>input->duration) return false;
        char *name=graph_name(input->name);if (!name) return false;
        if (!state) {
            if (*next==UINT64_MAX||a->state_count==FL_ANIMATION_GRAPH_LIMIT) {free(name);return false;}
            Fl_AnimationState *states=realloc(a->states,(a->state_count+1)*sizeof(*states));if (!states) {free(name);return false;}
            a->states=states;state=&states[a->state_count++];*state=(Fl_AnimationState){.id=(*next)++};
        }
        free((void *)state->name);state->name=name;state->duration=input->duration;state->loop=input->loop;*created=state->id;return true;
    }
    case FL_OP_ANIMATION_STATE_REMOVE:
        for (size_t i=0;i<a->state_count;++i) if (a->states[i].id==op->animation_state) {remove_state(a,i);return true;}return false;
    case FL_OP_ANIMATION_ENTRY:
        if (!fl_animation_state(a,op->animation_state)) return false;a->entry=op->animation_state;return true;
    case FL_OP_ANIMATION_PARAMETER_SET: {
        const Fl_AnimationParameter *input=op->animation_parameter_record;
        if (!input||input->kind<0||input->kind>FL_ANIM_TRIGGER||!isfinite(input->value)||
            (input->kind!=FL_ANIM_NUMBER&&input->value!=0&&input->value!=1)) return false;
        Fl_AnimationParameter *parameter=NULL;
        for (size_t i=0;i<a->parameter_count;++i) {
            if (a->parameters[i].id==op->animation_parameter) parameter=&a->parameters[i];
            else if (input->name&&!strcmp(a->parameters[i].name,input->name)) return false;
        }
        if (op->animation_parameter&&!parameter) return false;
        char *name=graph_name(input->name);if (!name) return false;
        if (!parameter) {
            if (*next==UINT64_MAX||a->parameter_count==FL_ANIMATION_GRAPH_LIMIT) {free(name);return false;}
            Fl_AnimationParameter *parameters=realloc(a->parameters,(a->parameter_count+1)*sizeof(*parameters));if (!parameters) {free(name);return false;}
            a->parameters=parameters;parameter=&parameters[a->parameter_count++];*parameter=(Fl_AnimationParameter){.id=(*next)++};
        }
        free((void *)parameter->name);Fl_Id id=parameter->id;*parameter=*input;parameter->id=id;parameter->name=name;*created=id;return true;
    }
    case FL_OP_ANIMATION_PARAMETER_REMOVE:
        for (size_t i=0;i<a->parameter_count;++i) if (a->parameters[i].id==op->animation_parameter) {
            free((void *)a->parameters[i].name);memmove(a->parameters+i,a->parameters+i+1,(a->parameter_count-i-1)*sizeof(*a->parameters));--a->parameter_count;
            size_t kept=0;for (size_t t=0;t<a->transition_count;++t) if (a->transitions[t].parameter!=op->animation_parameter) a->transitions[kept++]=a->transitions[t];
            a->transition_count=kept;return true;
        }
        return false;
    case FL_OP_ANIMATION_TRANSITION_SET: {
        if (!op->animation_transition_record) return false;
        Fl_AnimationTransition *transition=NULL;
        for (size_t i=0;i<a->transition_count;++i) if (a->transitions[i].id==op->animation_transition) transition=&a->transitions[i];
        if (op->animation_transition&&!transition) return false;
        if (!transition) {
            if (*next==UINT64_MAX||a->transition_count==FL_ANIMATION_GRAPH_LIMIT) return false;
            Fl_AnimationTransition *edges=realloc(a->transitions,(a->transition_count+1)*sizeof(*edges));if (!edges) return false;
            a->transitions=edges;transition=&edges[a->transition_count++];*transition=(Fl_AnimationTransition){.id=(*next)++};
        }
        Fl_Id id=transition->id;*transition=*op->animation_transition_record;transition->id=id;*created=id;return true;
    }
    case FL_OP_ANIMATION_TRANSITION_REMOVE:
        for (size_t i=0;i<a->transition_count;++i) if (a->transitions[i].id==op->animation_transition) {
            memmove(a->transitions+i,a->transitions+i+1,(a->transition_count-i-1)*sizeof(*a->transitions));--a->transition_count;return true;
        }
        return false;
    default:return false;
    }
}

size_t fl_animation_id_count(const Fl_Animation *a)
{
    size_t count=a->state_count+a->parameter_count+a->transition_count;
    for (size_t s=0;s<a->state_count;++s) {count+=a->states[s].track_count;for (size_t t=0;t<a->states[s].track_count;++t) count+=a->states[s].tracks[t].key_count;}
    return count;
}

void fl_animation_ids(const Fl_Animation *a,Fl_Id *ids)
{
    size_t count=0;
    for (size_t s=0;s<a->state_count;++s) {
        const Fl_AnimationState *state=&a->states[s];ids[count++]=state->id;
        for (size_t t=0;t<state->track_count;++t) {ids[count++]=state->tracks[t].id;for (size_t k=0;k<state->tracks[t].key_count;++k) ids[count++]=state->tracks[t].keys[k].id;}
    }
    for (size_t i=0;i<a->parameter_count;++i) ids[count++]=a->parameters[i].id;
    for (size_t i=0;i<a->transition_count;++i) ids[count++]=a->transitions[i].id;
}

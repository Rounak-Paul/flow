#include "fl_inspector.h"
#include "ui/fl_button.h"
#include "fl_file_browser.h"
#include "components/fl_script.h"
#include "doc/fl_json.h"
#include "schema/fl_parameters.h"
#include "fl_script_panel.h"
#include "components/fl_animation.h"
#include "components/fl_style.h"
#include "components/fl_video_clip.h"
#include "platform/fl_fs.h"
#include "style.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NAME,X,Y,WIDTH,HEIGHT,ROTATION,SCALE_X,SCALE_Y,FILL,STROKE,STROKE_WIDTH,FONT_SIZE,CONTENT,FONT_FILTER,OPACITY,RADIUS,SHADOW_BLUR,SHADOW_X,SHADOW_Y,SHADOW_COLOR,GRADIENT_TO,GRADIENT_ANGLE,CROP_X,CROP_Y,CROP_W,CROP_H,
       VIDEO_START,VIDEO_OFFSET,VIDEO_RATE,BLUR,FIELD_COUNT };
#define WEIGHT_OPTIONS 32
typedef struct Fl_InspectorField { struct Fl_Inspector *owner; char *value; bool dirty; } Fl_InspectorField;
typedef struct Fl_ParameterField { Fl_InspectorField input; char *name, *label; char type[12]; } Fl_ParameterField;
typedef struct Fl_FontChoice { struct Fl_Inspector *owner; size_t family; uint64_t generation; } Fl_FontChoice;
struct Fl_Inspector {
    Fl_SceneEdit *edit;
    Fl_Playback *playback;
    Fl_Fonts *fonts;
    Fl_EventBus *bus;
    Ca_Dock *dock;
    Ca_Window *window;
    Fl_Id script;                   /* draft: repository script this entity runs, or 0 for none */
    bool script_enabled, script_new;
    Fl_Id script_component, script_instances[65], loaded_script;
    Fl_TimeMapping script_time;
    Fl_InspectorField clock_fields[4];
    Fl_Id *clock_beats;
    size_t clock_beat_count;
    size_t script_instance_count;
    char script_instance_labels[65][96];
    Fl_Id *script_options;
    size_t script_option_count;
    Fl_ParameterField *parameters;
    size_t parameter_count;
    Ca_Label *status;
    Fl_Id scene,entity;
    uint64_t revision;
    size_t selection_count;
    Fl_InspectorField fields[FIELD_COUNT];
    double loaded_numbers[FIELD_COUNT];
    Fl_Typography typography;
    Fl_Id image;
    Fl_TextAlign align;
    bool loop,clip;
    Fl_FontChoice choices[FL_STYLE_FONT_LIST_LIMIT];
    unsigned weights[WEIGHT_OPTIONS];
    size_t weight_count;
    Fl_Id *image_options;
    size_t image_option_count;
    Fl_EventType project_event, time_event, playback_event;
    bool visible,dirty,refresh,applying;
    char message[160];
};

/* Replace owned field bytes without discarding the prior draft on allocation failure. */
static bool field_set(Fl_InspectorField *field,const char *text)
{
    char *copy=malloc(strlen(text)+1);if (!copy) return false;
    strcpy(copy,text);free(field->value);field->value=copy;return true;
}
/* Update the form's status without rebuilding focused inputs. */
static void status(Fl_Inspector *p,const char *message)
{
    snprintf(p->message,sizeof(p->message),"%s",message);
    if (p->status) ca_set_text(p->status,p->message);
}
/* Capture edits as drafts; model mutations happen only through Apply. */
static void edited(Ca_TextInput *input,void *data)
{
    Fl_InspectorField *field=data;
    if (field_set(field,ca_input_text(input))) {field->dirty=true;field->owner->dirty=true;status(field->owner,"Unsaved changes");}
}
/* Capture visibility in the same draft as transform/style edits. */
static void visibility_changed(Ca_Checkbox *checkbox,void *data)
{
    Fl_Inspector *p=data;p->visible=ca_checkbox_get(checkbox);p->dirty=true;status(p,"Unsaved changes");
}
/* Mark the form edited after a non-text control changed. */
static void drafted(Fl_Inspector *p)
{
    p->dirty=true;status(p,"Unsaved changes");
    if (p->dock) ca_dock_invalidate_panel(p->dock,"inspector");
}
/* Retain the script's enabled flag as part of the Inspector draft. */
static void script_enabled(Ca_Checkbox *checkbox, void *data)
{
    Fl_Inspector *p = data; p->script_enabled = ca_checkbox_get(checkbox); drafted(p);
}

/* Stage the script chosen from the repository list; the first entry detaches. */
static void script_chosen(Ca_Select *select, void *data)
{
    Fl_Inspector *p = data; int index = ca_select_get(select);
    if (index < 0 || (size_t)index >= p->script_option_count) return;
    p->script = p->script_options[index];
    if (p->script) p->script_enabled = true;
    drafted(p);
}

/* Select an attachment for editing; existing unsaved form edits must be applied first. */
static void script_instance_chosen(Ca_Select *select, void *data)
{
    Fl_Inspector *p=data; int index=ca_select_get(select);
    if (p->dirty || index<0 || (size_t)index>=p->script_instance_count) return;
    p->script_component=p->script_instances[index]; p->script_new=p->script_component==0;
    p->refresh=true;
    if (p->dock) ca_dock_invalidate_panel(p->dock,"inspector");
}

/* Stage the selected start/end beat while retaining explicit local offsets. */
static void clock_start_chosen(Ca_Select *select, void *data)
{
    Fl_Inspector *p=data; int i=ca_select_get(select);
    if (i<=0 || (size_t)i>=p->clock_beat_count) return;
    p->script_time.start.kind=FL_ANCHOR_BEAT; p->script_time.start.beat=p->clock_beats[i]; p->script_time.start.end=false; p->script_time.start.position=0; drafted(p);
}
static void clock_end_chosen(Ca_Select *select, void *data)
{
    Fl_Inspector *p=data; int i=ca_select_get(select);
    if (i<=0 || (size_t)i>=p->clock_beat_count) return;
    p->script_time.end.kind=FL_ANCHOR_BEAT; p->script_time.end.beat=p->clock_beats[i]; p->script_time.end.end=true; p->script_time.end.position=0; drafted(p);
}
static void clock_scope_changed(Ca_Checkbox *checkbox, void *data)
{
    Fl_Inspector *p=data; const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(p->edit),p->scene);
    bool bound=ca_checkbox_get(checkbox);
    if (bound && (!scene || !scene->beat_count)) {status(p,"Create a beat before binding this behavior."); return;}
    p->script_time.explicit_range=bound;
    if (bound && p->script_time.start.kind!=FL_ANCHOR_BEAT) p->script_time.start=(Fl_TimeAnchor){.kind=FL_ANCHOR_BEAT,.beat=scene->beats[0].id,.end=false,.offset=0};
    if (bound && p->script_time.end.kind!=FL_ANCHOR_BEAT) p->script_time.end=(Fl_TimeAnchor){.kind=FL_ANCHOR_BEAT,.beat=scene->beats[0].id,.end=true,.offset=0};
    drafted(p);
}
static void clock_mode_chosen(Ca_Select *select, void *data)
{
    Fl_Inspector *p=data; int i=ca_select_get(select); if (i<0 || i>1) return; p->script_time.mode=(Fl_RetimeMode)i; drafted(p);
}
static void clock_loop_changed(Ca_Checkbox *checkbox, void *data) {Fl_Inspector *p=data; p->script_time.loop=ca_checkbox_get(checkbox); drafted(p);}
static void clock_before_changed(Ca_Checkbox *checkbox, void *data) {Fl_Inspector *p=data; p->script_time.before=ca_checkbox_get(checkbox) ? FL_TIME_HOLD : FL_TIME_INACTIVE; drafted(p);}
static void clock_after_changed(Ca_Checkbox *checkbox, void *data) {Fl_Inspector *p=data; p->script_time.after=ca_checkbox_get(checkbox) ? FL_TIME_HOLD : FL_TIME_INACTIVE; drafted(p);}

/* Show the entity's script in the Scripts panel. */
static void script_edit(Ca_Button *button, void *data)
{
    (void)button; Fl_Inspector *p = data;
    if (p->script) fl_script_panel_request(p->bus, p->scene, p->script);
}

/* Pick a family from the filtered list; stale rows from an older font generation are ignored. */
static void family_chosen(Ca_Button *button,void *data)
{
    (void)button;Fl_FontChoice *choice=data;Fl_Inspector *p=choice->owner;
    const char *family=fl_fonts_family(p->fonts,choice->family,NULL);
    if (choice->generation!=fl_fonts_generation(p->fonts)||!family) return;
    snprintf(p->typography.family,sizeof(p->typography.family),"%s",family);
    drafted(p);
}
/* Map the weight dropdown back to the CSS weight captured at build time. */
static void weight_chosen(Ca_Select *select,void *data)
{
    Fl_Inspector *p=data;int index=ca_select_get(select);
    if (index>=0&&(size_t)index<p->weight_count) {p->typography.weight=p->weights[index];drafted(p);}
}
static void italic_changed(Ca_Checkbox *checkbox,void *data)
{
    Fl_Inspector *p=data;p->typography.italic=ca_checkbox_get(checkbox);drafted(p);
}
/* Map the alignment dropdown to the style's text alignment. */
static void align_chosen(Ca_Select *select,void *data)
{
    Fl_Inspector *p=data;int index=ca_select_get(select);
    if (index>=0&&index<FL_ALIGN_COUNT) {p->align=(Fl_TextAlign)index;drafted(p);}
}
static void loop_changed(Ca_Checkbox *checkbox,void *data)
{
    Fl_Inspector *p=data;p->loop=ca_checkbox_get(checkbox);drafted(p);
}
static void clip_changed(Ca_Checkbox *checkbox,void *data)
{
    Fl_Inspector *p=data;p->clip=ca_checkbox_get(checkbox);drafted(p);
}
/* Map the image dropdown back to the asset id captured at build time. */
static void image_chosen(Ca_Select *select,void *data)
{
    Fl_Inspector *p=data;int index=ca_select_get(select);
    if (index>=0&&(size_t)index<p->image_option_count) {p->image=p->image_options[index];drafted(p);}
}
/* Narrow the family list as the user types. */
static void filter_changed(Ca_TextInput *input,void *data)
{
    Fl_InspectorField *field=data;
    if (field_set(field,ca_input_text(input))&&field->owner->dock) ca_dock_invalidate_panel(field->owner->dock,"inspector");
}
/* Parse a complete finite number, rejecting partial edits and overflow. */
static bool number(const char *text,double *out)
{
    if (!text) return false;
    while (isspace((unsigned char)*text)) ++text;
    if (!*text) return false;
    char *end;errno=0;double value=strtod(text,&end);
    if (end==text||errno==ERANGE||!isfinite(value)) return false;
    while (isspace((unsigned char)*end)) ++end;
    if (*end) return false;
    *out=value;return true;
}
/* Parse sRGB hex colours with optional alpha. */
static bool color(const char *text,unsigned char rgba[4])
{
    if (!text) return false;
    if (*text=='#') ++text;
    size_t length=strlen(text);if (length!=6&&length!=8) return false;
    rgba[3]=255;
    for (size_t i=0;i<length/2;++i) {
        unsigned value=0;
        for (size_t j=0;j<2;++j) {
            int c=tolower((unsigned char)text[i*2+j]);
            if (!isxdigit(c)) return false;
            value=value*16+(unsigned)(c>='a' ? c-'a'+10 : c-'0');
        }
        rgba[i]=(unsigned char)value;
    }
    return true;
}
/* Commit the form in one operation, preserving component schemas the Inspector does not own. */
/* Serialize only edited parameter values; unchanged defaults continue to follow the Lua asset. */
static char *parameter_drafts(Fl_Inspector *p, const Fl_Script *script)
{
    Fl_Json context={0};
    if (p->script!=p->loaded_script) return cJSON_PrintUnformatted(&(cJSON){.type=cJSON_Object});
    cJSON *values=script->parameters ? fl_json_parse(&context,script->parameters,script->parameters_size) : cJSON_CreateObject();
    if (!values) return NULL;
    for (size_t i=0;i<p->parameter_count;++i) {
        Fl_ParameterField *field=&p->parameters[i]; if (!field->input.dirty) continue;
        cJSON *value=NULL;
        if (!strcmp(field->type,"string")) value=cJSON_CreateString(field->input.value);
        else if (!strcmp(field->type,"boolean")) value=cJSON_CreateBool(!strcmp(field->input.value,"true"));
        else if (!strcmp(field->type,"number") || !strcmp(field->type,"integer")) {
            double n;
            if (!number(field->input.value,&n) || (!strcmp(field->type,"integer") && (floor(n)!=n || fabs(n)>9007199254740991.0))) {cJSON_Delete(values); return NULL;}
            value=cJSON_CreateNumber(n);
        } else value=fl_json_parse(&context,field->input.value,strlen(field->input.value));
        if (!value) {cJSON_Delete(values); return NULL;}
        cJSON_DeleteItemFromObjectCaseSensitive(values,field->name);
        if (!fl_json_add(&context,values,field->name,value)) {cJSON_Delete(values); return NULL;}
    }
    const Fl_SceneState *state=fl_scene_edit_state(p->edit);
    const cJSON *schema=state ? fl_script_runtime_parameter_schema(fl_scene_state_scripts(state),p->script_component) : NULL;
    if (schema && !fl_parameters_resolve(&context,schema,values,NULL)) {status(p,context.error); cJSON_Delete(values); return NULL;}
    char *text=context.error[0] ? NULL : cJSON_PrintUnformatted(values); cJSON_Delete(values); return text;
}

static void apply(Ca_Button *button,void *data)
{
    (void)button;Fl_Inspector *p=data;
    const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(p->edit),p->scene);
    const Fl_Entity *entity=fl_scene_entity(scene,p->entity);
    if (!entity||scene->revision!=p->revision) {status(p,"Scene changed. Revert to reload before applying.");return;}
    double values[7];
    for (size_t i=0;i<7;++i) if (!number(p->fields[X+i].value,&values[i])) {status(p,"Enter finite numbers for every transform field.");return;}
    for (size_t i=0;i<7;++i) if (!p->fields[X+i].dirty) values[i]=p->loaded_numbers[X+i];
    Fl_Transform transform={values[0],values[1],values[2],values[3],values[4]*3.14159265358979323846/180,values[5],values[6]};
    if (transform.width<0||transform.height<0) {status(p,"Width and height must be non-negative.");return;}
    const Fl_WidgetDef *def=fl_widget_for_entity(entity);Fl_Appearance appearance=fl_widget_appearance(entity);
    if ((def&&def->has_fill&&!color(p->fields[FILL].value,appearance.fill))||
        (def&&def->has_stroke&&(!color(p->fields[STROKE].value,appearance.stroke)||!number(p->fields[STROKE_WIDTH].value,&appearance.stroke_width)))||
        (def&&def->has_text&&!number(p->fields[FONT_SIZE].value,&appearance.font_size))) {status(p,"Check colour and appearance values.");return;}
    if (!p->fields[STROKE_WIDTH].dirty) appearance.stroke_width=p->loaded_numbers[STROKE_WIDTH];
    if (!p->fields[FONT_SIZE].dirty) appearance.font_size=p->loaded_numbers[FONT_SIZE];
    if (!p->fields[NAME].value||!p->fields[NAME].value[0]) {status(p,"Name cannot be empty.");return;}
    double opacity;
    if (!number(p->fields[OPACITY].value,&opacity)||opacity<0||opacity>1) {status(p,"Opacity is between 0 and 1.");return;}
    if (!p->fields[OPACITY].dirty) opacity=p->loaded_numbers[OPACITY];
    unsigned char bytes[56],style[24],typography[3+FL_TYPOGRAPHY_FAMILY_MAX],image[8],alpha[8];
    fl_transform_encode(&transform,bytes);fl_appearance_encode(&appearance,style);fl_opacity_encode(opacity,alpha);
    unsigned char style_bytes[FL_STYLE_BYTES],clip_bytes[FL_VIDEO_CLIP_BYTES];
    Fl_Component components[9]={{.type=FL_COMPONENT_TRANSFORM, .version=1, .size=56, .data=bytes},{.type=FL_COMPONENT_OPACITY, .version=1, .size=8, .data=alpha}};size_t count=2;
    if (def&&def->kind!=FL_WIDGET_GROUP&&!def->has_image&&!def->has_video) components[count++]=(Fl_Component){.type = FL_COMPONENT_APPEARANCE, .version = 1, .size = 24, .data = style};
    if (def&&def->has_text) {
        size_t size=fl_typography_encode(&p->typography,typography);
        if (!size) {status(p,"Choose a font family.");return;}
        components[count++]=(Fl_Component){.type = FL_COMPONENT_TEXT, .version = 1, .size = strlen(p->fields[CONTENT].value), .data = (const unsigned char *)p->fields[CONTENT].value};
        components[count++]=(Fl_Component){.type = FL_COMPONENT_TYPOGRAPHY, .version = 1, .size = size, .data = typography};
    }
    if (def&&def->has_image) {
        if (!fl_project_asset(fl_scene_edit_project(p->edit),p->image)) {status(p,"Choose an image asset.");return;}
        fl_image_asset_encode(p->image,image);
        components[count++]=(Fl_Component){.type = FL_COMPONENT_IMAGE, .version = 1, .size = 8, .data = image};
    }
    Fl_Style look=fl_style_default();
    if (def) {
        double v[4];
        bool ok=number(p->fields[BLUR].value,&look.blur)&&number(p->fields[RADIUS].value,&look.radius)&&number(p->fields[SHADOW_BLUR].value,&look.shadow_blur)&&
            number(p->fields[SHADOW_X].value,&look.shadow_x)&&number(p->fields[SHADOW_Y].value,&look.shadow_y)&&number(p->fields[GRADIENT_ANGLE].value,&v[0]);
        look.gradient_angle=v[0]*3.14159265358979323846/180;
        for (int i=0;ok&&i<4;++i) ok=number(p->fields[CROP_X+i].value,&look.crop[i]);
        ok=ok&&color(p->fields[SHADOW_COLOR].value,look.shadow_color);
        const char *to=p->fields[GRADIENT_TO].value;
        look.gradient=to&&to[0];
        if (ok&&look.gradient) ok=color(to,look.gradient_to);
        look.align=p->align;look.clip=p->clip;
        unsigned char probe[FL_STYLE_BYTES];fl_style_encode(&look,probe);
        Fl_Component probe_component={.type = FL_COMPONENT_STYLE, .version = 1, .size = FL_STYLE_BYTES, .data = probe};
        if (!ok||!fl_widget_component_valid(&probe_component)) {status(p,"Check radius, blur, shadow, gradient and crop values.");return;}
        Fl_Style plain=fl_style_default();unsigned char plain_bytes[FL_STYLE_BYTES];fl_style_encode(&plain,plain_bytes);
        if (memcmp(probe,plain_bytes,sizeof(probe))||fl_entity_component(entity,FL_COMPONENT_STYLE)) {
            memcpy(style_bytes,probe,sizeof(probe));components[count++]=(Fl_Component){.type = FL_COMPONENT_STYLE, .version = 1, .size = FL_STYLE_BYTES, .data = style_bytes};
        }
    }
    if (def&&def->has_video) {
        Fl_VideoClip clip={.rate=1};fl_video_clip_decode(fl_entity_component(entity,FL_COMPONENT_VIDEO),&clip);
        double start,offset;
        if (!number(p->fields[VIDEO_START].value,&start)||!number(p->fields[VIDEO_OFFSET].value,&offset)||!number(p->fields[VIDEO_RATE].value,&clip.rate)||
            offset<0||!(clip.rate>0&&clip.rate<=16)) {status(p,"Start and offset are seconds (offset not negative); rate is above 0 and at most 16.");return;}
        clip.start=(Fl_Tick)llround(start*FL_TICKS_PER_SECOND);clip.offset=(Fl_Tick)llround(offset*FL_TICKS_PER_SECOND);clip.loop=p->loop;
        fl_video_clip_encode(&clip,clip_bytes);components[count++]=(Fl_Component){.type = FL_COMPONENT_VIDEO, .version = 1, .size = FL_VIDEO_CLIP_BYTES, .data = clip_bytes};
    }
    Fl_Component script = {0};
    const bool had_script=fl_entity_component_instance(entity,p->script_component)!=NULL;
    if (p->script) {
        const Fl_Component *attachment=fl_entity_component_instance(entity,p->script_component);
        Fl_Script configuration={.script=p->script,.enabled=p->script_enabled};
        if (attachment) {
            fl_script_decode(attachment,&configuration);
            if (configuration.script!=p->script) {configuration.parameters=NULL; configuration.parameters_size=0; configuration.bindings=NULL; configuration.bindings_size=0;}
            configuration.script=p->script; configuration.enabled=p->script_enabled;
        }
        configuration.time=p->script_time;
        double seconds;
        if (p->clock_fields[0].dirty) {
            if (!p->clock_fields[0].value[0]) configuration.time.duration=0;
            else if (!number(p->clock_fields[0].value,&seconds) || seconds<0 || !fl_time_from_seconds(seconds,&configuration.time.duration)) {status(p,"Invalid canonical duration."); return;}
        }
        if (p->clock_fields[1].dirty && !number(p->clock_fields[1].value,&configuration.time.rate)) {status(p,"Invalid behavior rate."); return;}
        if (p->clock_fields[2].dirty && (!number(p->clock_fields[2].value,&seconds) || !fl_time_from_seconds(seconds,&configuration.time.start.offset))) {status(p,"Invalid start offset."); return;}
        if (p->clock_fields[3].dirty && (!number(p->clock_fields[3].value,&seconds) || !fl_time_from_seconds(seconds,&configuration.time.end.offset))) {status(p,"Invalid end offset."); return;}
        char *parameters=parameter_drafts(p,&configuration);
        if (!parameters) {status(p,"Invalid behavior parameter value."); return;}
        configuration.parameters=parameters; configuration.parameters_size=strlen(parameters);
        bool encoded=fl_script_encode(&configuration,&script); cJSON_free(parameters);
        if (!encoded) {status(p,"Invalid Lua script component."); return;}
        if (attachment) script.id=attachment->id;
        components[count++] = script;
    }
    p->applying=true;
    bool begun=fl_project_edit_begin(fl_scene_edit_project(p->edit),"Edit component properties",false);
    bool ok=begun && fl_project_apply(fl_scene_edit_project(p->edit),&(Fl_Op){.kind=FL_OP_ENTITY_CONFIGURE,.scene=p->scene,.entity=p->entity,
        .name=p->fields[NAME].value,.visible=p->visible,.components=components,.component_count=count,
        .keyed=true,.tick=fl_scene_edit_time(p->edit)},NULL);
    if (ok&&!p->script&&had_script) ok=fl_project_apply(fl_scene_edit_project(p->edit),&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=p->scene,
        .entity=p->entity,.component={.type=FL_COMPONENT_SCRIPT,.id=p->script_component}},NULL);
    if (begun) {if (ok) ok=fl_project_edit_commit(fl_scene_edit_project(p->edit)); else fl_project_edit_cancel(fl_scene_edit_project(p->edit));}
    p->applying=false;
    free((void *)script.data);
    if (!ok) {const char *error=fl_project_diagnostic(fl_scene_edit_project(p->edit)); status(p,error[0] ? error : "Invalid values or insufficient memory. Changes were not applied.");return;}
    p->dirty=false;p->refresh=true;status(p,"Saved");
    if (p->dock) ca_dock_invalidate_panel(p->dock,"inspector");
}
/* Discard drafts explicitly and reload the current entity on the next build. */
static void revert(Ca_Button *button,void *data)
{
    (void)button;Fl_Inspector *p=data;p->dirty=false;p->refresh=true;status(p,"");
    if (p->dock) ca_dock_invalidate_panel(p->dock,"inspector");
}
/* Observe selection changes and avoid silently overwriting stale property drafts. */
static bool changed(const Fl_Event *event,void *data)
{
    Fl_Inspector *p=data;if (p->applying) return false;
    if (event->type==p->project_event) {
        Fl_OpKind kind=((const Fl_ProjectChange *)event->payload)->kind;
        if (kind==FL_OP_ASSET_ADD||kind==FL_OP_ASSET_REMOVE||kind==FL_OP_ASSET_RENAME) {
            if (p->dock) ca_dock_invalidate_panel(p->dock,"inspector");
            return false;
        }
    }
    Fl_Id scene=fl_scene_edit_scene(p->edit),entity=fl_scene_edit_primary(p->edit);
    if (p->scene!=scene||p->entity!=entity||p->selection_count!=fl_scene_edit_count(p->edit)) {p->dirty=false;p->refresh=true;}
    else if (p->dirty) {status(p,"Scene changed. Revert to reload before applying.");return false;}
    else p->refresh=true;
    if (p->dock) ca_dock_invalidate_panel(p->dock,"inspector");
    return false;
}

/* Release draft parameter strings independently of runtime metadata lifetime. */
static void clear_parameters(Fl_Inspector *p)
{
    for (size_t i=0;i<p->parameter_count;++i) {free(p->parameters[i].name); free(p->parameters[i].label); free(p->parameters[i].input.value);}
    free(p->parameters); p->parameters=NULL; p->parameter_count=0;
}

/* Order native controls by parameter name, independent of Lua table iteration. */
static int compare_parameters(const void *a, const void *b)
{
    const cJSON *const *x=a,*const *y=b; return strcmp((*x)->string,(*y)->string);
}

/* Copy exposed definitions and effective values into editable native drafts. */
static bool load_parameters(Fl_Inspector *p, const Fl_SceneState *state, const Fl_Script *script)
{
    clear_parameters(p);
    const cJSON *schema=state ? fl_script_runtime_parameter_schema(fl_scene_state_scripts(state),p->script_component) : NULL;
    if (!schema) return true;
    size_t count=0; const cJSON *ordered[64];
    for (const cJSON *item=schema->child;item && count<64;item=item->next) ordered[count++]=item;
    if (count) qsort(ordered,count,sizeof(*ordered),compare_parameters);
    p->parameters=count ? calloc(count,sizeof(*p->parameters)) : NULL;
    if (count && !p->parameters) return false;
    p->parameter_count=count;
    Fl_Json context={0}; cJSON *values=fl_json_parse(&context,script->parameters,script->parameters_size);
    if (!values) return false;
    bool ok=true;
    for (size_t i=0;i<count && ok;++i) {
        const cJSON *spec=ordered[i], *type=cJSON_GetObjectItemCaseSensitive(spec,"type"), *label=cJSON_GetObjectItemCaseSensitive(spec,"label"),
            *value=cJSON_GetObjectItemCaseSensitive(values,spec->string);
        Fl_ParameterField *field=&p->parameters[i]; field->input.owner=p;
        if (!cJSON_IsString(type) || strlen(type->valuestring)>=sizeof(field->type)) {ok=false; break;}
        strcpy(field->type,type->valuestring); field->name=fl_json_string(&context,&(cJSON){.type=cJSON_String,.valuestring=spec->string});
        field->label=fl_json_string(&context,cJSON_IsString(label) ? label : &(cJSON){.type=cJSON_String,.valuestring=spec->string});
        if (!value) value=cJSON_GetObjectItemCaseSensitive(spec,"default");
        char *formatted=value ? cJSON_PrintUnformatted(value) : NULL;
        const char *text=cJSON_IsString(value) ? value->valuestring : formatted ? formatted : "";
        ok=field->name && field->label && field_set(&field->input,text); cJSON_free(formatted);
    }
    cJSON_Delete(values); return ok;
}

/* Snapshot current values into controller-owned strings; no document pointers survive a build. */
static bool load(Fl_Inspector *p,const Fl_Scene *scene,const Fl_Entity *entity)
{
    const Fl_SceneState *state=fl_scene_edit_state(p->edit);
    Fl_Script attached;
    bool scripted=false;
    for (size_t i=0;i<entity->component_count;++i) if (fl_script_decode(&entity->components[i],&attached) && attached.enabled) scripted=true;
    bool evaluated=!scripted&&state&&state->scene==scene->id&&state->count>=scene->entity_count;
    const Fl_EntityState *now=evaluated ? &state->entities[entity-scene->entities] : NULL;
    Fl_Transform transform={.scale_x=1,.scale_y=1};
    if (now) transform=now->transform;
    else fl_transform_decode(fl_entity_component(entity,FL_COMPONENT_TRANSFORM),&transform);
    double values[7]={transform.x,transform.y,transform.width,transform.height,transform.rotation*180/3.14159265358979323846,transform.scale_x,transform.scale_y};
    if (!field_set(&p->fields[NAME],entity->name)) return false;
    char text[96];
    for (size_t i=0;i<7;++i) {p->loaded_numbers[X+i]=values[i];snprintf(text,sizeof(text),"%.9g",values[i]);if (!field_set(&p->fields[X+i],text)) return false;}
    Fl_Appearance a=now ? now->appearance : fl_widget_appearance(entity);
    p->loaded_numbers[OPACITY]=now ? now->opacity : fl_widget_opacity(entity);
    snprintf(text,sizeof(text),"%.4g",p->loaded_numbers[OPACITY]);if (!field_set(&p->fields[OPACITY],text)) return false;
    snprintf(text,sizeof(text),"#%02X%02X%02X%02X",a.fill[0],a.fill[1],a.fill[2],a.fill[3]);if (!field_set(&p->fields[FILL],text)) return false;
    snprintf(text,sizeof(text),"#%02X%02X%02X%02X",a.stroke[0],a.stroke[1],a.stroke[2],a.stroke[3]);if (!field_set(&p->fields[STROKE],text)) return false;
    p->loaded_numbers[STROKE_WIDTH]=a.stroke_width;p->loaded_numbers[FONT_SIZE]=a.font_size;
    snprintf(text,sizeof(text),"%.9g",a.stroke_width);if (!field_set(&p->fields[STROKE_WIDTH],text)) return false;
    snprintf(text,sizeof(text),"%.9g",a.font_size);if (!field_set(&p->fields[FONT_SIZE],text)) return false;
    const Fl_Component *content=fl_entity_component(entity,FL_COMPONENT_TEXT);
    char *copy=malloc((content ? content->size : 0)+1);if (!copy) return false;
    if (content&&content->size) memcpy(copy,content->data,content->size);
    copy[content ? content->size : 0]=0;free(p->fields[CONTENT].value);p->fields[CONTENT].value=copy;
    p->typography=fl_widget_typography(entity);
    p->image=0;fl_image_asset_decode(fl_entity_component(entity,FL_COMPONENT_IMAGE),&p->image);
    Fl_Style look=fl_entity_style(entity);
    if (now) memcpy(look.crop,now->crop,sizeof(look.crop));
    p->align=look.align;p->clip=look.clip;
    p->loaded_numbers[BLUR]=look.blur;snprintf(text,sizeof(text),"%.9g",look.blur);if (!field_set(&p->fields[BLUR],text)) return false;
    const double style_values[]={look.radius,look.shadow_blur,look.shadow_x,look.shadow_y};
    for (size_t i=0;i<4;++i) {p->loaded_numbers[RADIUS+i]=style_values[i];snprintf(text,sizeof(text),"%.9g",style_values[i]);if (!field_set(&p->fields[RADIUS+i],text)) return false;}
    snprintf(text,sizeof(text),"#%02X%02X%02X%02X",look.shadow_color[0],look.shadow_color[1],look.shadow_color[2],look.shadow_color[3]);if (!field_set(&p->fields[SHADOW_COLOR],text)) return false;
    if (look.gradient) snprintf(text,sizeof(text),"#%02X%02X%02X%02X",look.gradient_to[0],look.gradient_to[1],look.gradient_to[2],look.gradient_to[3]);else text[0]=0;
    if (!field_set(&p->fields[GRADIENT_TO],text)) return false;
    snprintf(text,sizeof(text),"%.9g",look.gradient_angle*180/3.14159265358979323846);if (!field_set(&p->fields[GRADIENT_ANGLE],text)) return false;
    for (size_t i=0;i<4;++i) {p->loaded_numbers[CROP_X+i]=look.crop[i];snprintf(text,sizeof(text),"%.9g",look.crop[i]);if (!field_set(&p->fields[CROP_X+i],text)) return false;}
    Fl_VideoClip clip={.rate=1};fl_video_clip_decode(fl_entity_component(entity,FL_COMPONENT_VIDEO),&clip);
    p->loop=clip.loop;
    const double clip_values[]={(double)clip.start/FL_TICKS_PER_SECOND,(double)clip.offset/FL_TICKS_PER_SECOND,clip.rate};
    for (size_t i=0;i<3;++i) {snprintf(text,sizeof(text),"%.9g",clip_values[i]);if (!field_set(&p->fields[VIDEO_START+i],text)) return false;}
    Fl_Script script;
    bool same=p->entity==entity->id && p->scene==scene->id;
    if (!same) p->script_new=false;
    const Fl_Component *attachment=same ? fl_entity_component_instance(entity,p->script_component) : NULL;
    if (!attachment && !p->script_new) attachment=fl_entity_component(entity,FL_COMPONENT_SCRIPT);
    const bool has_script=fl_script_decode(attachment,&script);
    p->script_component=has_script ? attachment->id : 0;
    p->script=has_script ? script.script : 0;
    p->script_enabled=has_script&&script.enabled;
    p->loaded_script=p->script; p->script_time=has_script ? script.time : fl_time_mapping_default();
    const double clocks[]={(double)p->script_time.duration/FL_TICKS_PER_SECOND,p->script_time.rate,(double)p->script_time.start.offset/FL_TICKS_PER_SECOND,(double)p->script_time.end.offset/FL_TICKS_PER_SECOND};
    for (size_t i=0;i<4;++i) {
        p->clock_fields[i].owner=p;
        if (!i && !p->script_time.duration) text[0]=0; else snprintf(text,sizeof(text),"%.17g",clocks[i]);
        if (!field_set(&p->clock_fields[i],text)) return false;
        p->clock_fields[i].dirty=false;
    }
    if (has_script) {if (!load_parameters(p,state,&script)) return false;} else clear_parameters(p);
    for (size_t i=0;i<FIELD_COUNT;++i) p->fields[i].dirty=false;
    p->visible=entity->visible;p->scene=scene->id;p->entity=entity->id;p->revision=scene->revision;p->refresh=false;return true;
}
/* Emit one labelled retained draft field with its stable callback binding. */
static void field(Fl_Inspector *p,int index,const char *label,bool numeric)
{
    static const int animated[FIELD_COUNT]={-1,FL_PROP_X,FL_PROP_Y,FL_PROP_WIDTH,FL_PROP_HEIGHT,FL_PROP_ROTATION,FL_PROP_SCALE_X,
        FL_PROP_SCALE_Y,FL_PROP_FILL,FL_PROP_STROKE,FL_PROP_STROKE_WIDTH,FL_PROP_FONT_SIZE,-1,-1,FL_PROP_OPACITY,-1,-1,-1,-1,-1,-1,-1,
        FL_PROP_CROP_X,FL_PROP_CROP_Y,FL_PROP_CROP_W,FL_PROP_CROP_H,-1,-1,-1,-1};
    const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(p->edit),p->scene);
    bool keyed=animated[index]>=0&&fl_scene_track(scene,p->entity,(Fl_AnimProperty)animated[index]);
    char text[64];snprintf(text,sizeof(text),keyed ? "%s " CA_ICON_NF_FA_DIAMOND : "%s",label);
    ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW});
    ca_text(&(Ca_TextDesc){.text=text,.style=keyed ? FL_CLASS_PROPERTY_LABEL_KEYED : FL_CLASS_PROPERTY_LABEL});
    char id[32];snprintf(id,sizeof(id),"property-%d",index);
    ca_input(&(Ca_InputDesc){.text=p->fields[index].value,.id=id,.style=FL_CLASS_PROPERTY_INPUT,
        .input_mode=numeric ? CA_INPUT_FLOAT : CA_INPUT_TEXT,.on_change=edited,.change_data=&p->fields[index]});
    ca_div_end();
}
/* Refresh the family list and missing-font notice without disturbing drafts. */
static bool fonts_changed(const Fl_Event *event,void *data)
{
    Fl_Inspector *p=data;const Fl_FontsChanged *change=event->payload;
    if (change->fonts==p->fonts&&p->dock) ca_dock_invalidate_panel(p->dock,"inspector");
    return false;
}
/* Show values at the new time unless playing or holding unsaved drafts; refresh when playback stops. */
static bool timing_changed(const Fl_Event *event,void *data)
{
    Fl_Inspector *p=data;
    if (event->type==p->time_event&&(fl_playback_playing(p->playback)||p->dirty)) return false;
    if (event->type==p->playback_event&&((const Fl_PlaybackChange *)event->payload)->playing) return false;
    if (!p->dirty) p->refresh=true;
    if (p->dock) ca_dock_invalidate_panel(p->dock,"inspector");
    return false;
}
Fl_Inspector *fl_inspector_create(Fl_SceneEdit *edit,Fl_Fonts *fonts,Fl_Playback *playback,Fl_EventBus *bus)
{
    if (!edit||!fonts||!playback||!bus) return NULL;
    Fl_Inspector *p=calloc(1,sizeof(*p));if (!p) return NULL;
    p->edit=edit;p->fonts=fonts;p->playback=playback;p->bus=bus;p->refresh=true;
    for (int i=0;i<FIELD_COUNT;++i) p->fields[i].owner=p;
    for (size_t i=0;i<FL_STYLE_FONT_LIST_LIMIT;++i) p->choices[i].owner=p;
    p->project_event=fl_event_type_find(bus,FL_EVENT_PROJECT_CHANGED);
    p->time_event=fl_event_type_find(bus,FL_EVENT_TIME_CHANGED);
    p->playback_event=fl_event_type_find(bus,FL_EVENT_PLAYBACK_CHANGED);
    for (int i=0;i<2;++i) if (!fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=i ? p->playback_event : p->time_event,
        .fn=timing_changed,.user_data=p,.owner=p}).generation) {fl_inspector_destroy(p);return NULL;}
    const char *events[]={FL_EVENT_PROJECT_CHANGED,FL_EVENT_SELECTION_CHANGED};
    for (size_t i=0;i<2;++i) if (!fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=fl_event_type_find(bus,events[i]),
        .fn=changed,.user_data=p,.owner=p}).generation) {fl_inspector_destroy(p);return NULL;}
    if (!fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=fl_event_type_find(bus,FL_EVENT_FONTS_CHANGED),
        .fn=fonts_changed,.user_data=p,.owner=p}).generation) {fl_inspector_destroy(p);return NULL;}
    return p;
}
void fl_inspector_teardown(void *data) {Fl_Inspector *p=data;p->dock=NULL;p->status=NULL;p->window=NULL;}
void fl_inspector_destroy(Fl_Inspector *p)
{
    if (!p) return;
    fl_event_unsubscribe_owner(p->bus,p);for (int i=0;i<FIELD_COUNT;++i) free(p->fields[i].value);
    for (size_t i=0;i<4;++i) free(p->clock_fields[i].value);
    free(p->clock_beats);clear_parameters(p);free(p->image_options);free(p->script_options);free(p);
}
/* Describe a CSS weight with its conventional name. */
static void weight_label(unsigned weight,const char *style,char *out,size_t capacity)
{
    static const char *names[]={"Thin","Extra Light","Light","Regular","Medium","Semibold","Bold","Extra Bold","Black"};
    unsigned slot=weight<100 ? 0 : weight>900 ? 8 : (weight+50)/100-1;
    snprintf(out,capacity,"%u  %s",weight,style&&style[0] ? style : names[slot>8 ? 8 : slot]);
}
/* Emit family search/list, weight and italic controls for the drafted typography. */
static void typography_section(Fl_Inspector *p)
{
    ca_text(&(Ca_TextDesc){.text="Typography",.style=FL_CLASS_HEADING});
    Fl_FontMatch match=fl_fonts_match(p->fonts,&p->typography);
    char line[FL_TYPOGRAPHY_FAMILY_MAX+96];
    ca_text(&(Ca_TextDesc){.text=p->typography.family,.style=FL_CLASS_LABEL,.wrap=true});
    if (!match.found) {
        snprintf(line,sizeof(line),"Not installed, using %s",FL_TYPOGRAPHY_DEFAULT_FAMILY);
        ca_text(&(Ca_TextDesc){.text=line,.style=FL_CLASS_WARNING,.wrap=true});
    }
    if (fl_fonts_scanning(p->fonts)) ca_text(&(Ca_TextDesc){.text="Loading fonts...",.style=FL_CLASS_MUTED});
    ca_input(&(Ca_InputDesc){.text=p->fields[FONT_FILTER].value,.placeholder="Search",.id="property-font-filter",
        .style=FL_CLASS_INPUT,.on_change=filter_changed,.change_data=&p->fields[FONT_FILTER]});
    ca_div_begin(&(Ca_DivDesc){.direction=CA_VERTICAL,.style=FL_CLASS_FONT_LIST,.id="font-list"});
    uint64_t generation=fl_fonts_generation(p->fonts);
    size_t shown=0,matching=0,families=fl_fonts_family_count(p->fonts);
    const char *query=p->fields[FONT_FILTER].value;
    for (size_t i=0;i<families;++i) {
        Fl_FontSource source;const char *family=fl_fonts_family(p->fonts,i,&source);
        if (query&&query[0]) {
            bool hit=false;
            for (const char *c=family;*c&&!hit;++c) {
                const char *a=c,*b=query;
                while (*a&&*b&&tolower((unsigned char)*a)==tolower((unsigned char)*b)) {++a;++b;}
                hit=!*b;
            }
            if (!hit) continue;
        }
        ++matching;
        if (shown==FL_STYLE_FONT_LIST_LIMIT) continue;
        Fl_FontChoice *choice=&p->choices[shown++];choice->family=i;choice->generation=generation;
        char id[48];snprintf(id,sizeof(id),"font-%zu",i);
        ca_btn_begin(&(Ca_BtnDesc){.direction=CA_HORIZONTAL,.id=id,.on_click=family_chosen,.click_data=choice,
            .style=!fl_font_family_compare(family,p->typography.family) ? FL_CLASS_FONT_ROW_SELECTED : FL_CLASS_FONT_ROW});
        ca_text(&(Ca_TextDesc){.text=family,.style=FL_CLASS_LABEL});
        if (source!=FL_FONT_SOURCE_SYSTEM) ca_text(&(Ca_TextDesc){.text=source==FL_FONT_SOURCE_PROJECT ? "Project" : "Bundled",.style=FL_CLASS_MUTED});
        ca_btn_end();
    }
    ca_div_end();
    if (matching>shown) {
        snprintf(line,sizeof(line),"%zu of %zu",shown,matching);
        ca_text(&(Ca_TextDesc){.text=line,.style=FL_CLASS_MUTED,.wrap=true});
    }
    const Fl_FontFace *faces;size_t face_count=fl_fonts_faces(p->fonts,p->typography.family,&faces);
    char labels[WEIGHT_OPTIONS][64];const char *options[WEIGHT_OPTIONS];
    p->weight_count=0;int selected=0;
    for (size_t i=0;i<face_count&&p->weight_count<WEIGHT_OPTIONS;++i) {
        if (p->weight_count&&p->weights[p->weight_count-1]==faces[i].weight) continue;
        const char *style=faces[i].italic ? NULL : faces[i].style;
        weight_label(faces[i].weight,style,labels[p->weight_count],sizeof(labels[0]));
        p->weights[p->weight_count++]=faces[i].weight;
    }
    if (!p->weight_count) for (unsigned w=100;w<=900;w+=100) {
        weight_label(w,NULL,labels[p->weight_count],sizeof(labels[0]));p->weights[p->weight_count++]=w;
    }
    bool listed=false;
    for (size_t i=0;i<p->weight_count;++i) if (p->weights[i]==p->typography.weight) {selected=(int)i;listed=true;}
    if (!listed&&p->weight_count<WEIGHT_OPTIONS) {
        weight_label(p->typography.weight,"(not available)",labels[p->weight_count],sizeof(labels[0]));
        selected=(int)p->weight_count;p->weights[p->weight_count++]=p->typography.weight;
    }
    for (size_t i=0;i<p->weight_count;++i) options[i]=labels[i];
    ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW});
    ca_text(&(Ca_TextDesc){.text="Weight",.style=FL_CLASS_PROPERTY_LABEL});
    ca_select(&(Ca_SelectDesc){.options=options,.option_count=(int)p->weight_count,.selected=selected,.id="property-weight",
        .style=FL_CLASS_PROPERTY_SELECT,.on_change=weight_chosen,.change_data=p});
    ca_div_end();
    ca_checkbox(&(Ca_CheckboxDesc){.text="Italic",.checked=p->typography.italic,.on_change=italic_changed,.change_data=p});
}
/* Emit the image asset chooser. */
static void image_section(Fl_Inspector *p)
{
    Fl_Project *project=fl_scene_edit_project(p->edit);
    ca_text(&(Ca_TextDesc){.text="Image",.style=FL_CLASS_HEADING});
    size_t total=fl_project_asset_count(project),count=0;
    Fl_Id *ids=total ? malloc(total*sizeof(*ids)) : NULL;
    const char **options=total ? malloc(total*sizeof(*options)) : NULL;
    if (total&&(!ids||!options)) {free(ids);free(options);ca_text(&(Ca_TextDesc){.text="Out of memory.",.style=FL_CLASS_MUTED});return;}
    int selected=-1;
    for (size_t i=0;i<total;++i) {
        const Fl_Asset *a=fl_project_asset_at(project,i);
        if (a->kind!=FL_ASSET_IMAGE) continue;
        if (a->id==p->image) selected=(int)count;
        ids[count]=a->id;options[count++]=a->name;
    }
    free(p->image_options);p->image_options=ids;p->image_option_count=count;
    ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW});
    ca_text(&(Ca_TextDesc){.text="Asset",.style=FL_CLASS_PROPERTY_LABEL});
    ca_select(&(Ca_SelectDesc){.options=options,.option_count=(int)count,.selected=selected<0 ? 0 : selected,.id="property-image",
        .style=FL_CLASS_PROPERTY_SELECT,.on_change=image_chosen,.change_data=p,.disabled=!count});
    ca_div_end();
    free(options);
    const Fl_Asset *a=fl_project_asset(project,p->image);
    char line[96];
    if (a&&a->width&&a->height) {
        snprintf(line,sizeof(line),"%llu x %llu px",(unsigned long long)a->width,(unsigned long long)a->height);
        ca_text(&(Ca_TextDesc){.text=line,.style=FL_CLASS_MUTED});
    }
}
/* Emit appearance extras: corner radius, gradient, shadow, text alignment, crop window and video playback. */
static void style_section(Fl_Inspector *p,const Fl_WidgetDef *def)
{
    if (!def) return;
    ca_text(&(Ca_TextDesc){.text="Style",.style=FL_CLASS_HEADING});
    bool box=def->kind==FL_WIDGET_RECTANGLE||def->kind==FL_WIDGET_GROUP||def->has_image||def->has_video,shape=def->kind==FL_WIDGET_RECTANGLE||def->kind==FL_WIDGET_ELLIPSE,filled=shape||fl_widget_is_polygon(def->kind);
    if (box) field(p,RADIUS,"Radius",true);
    ca_checkbox(&(Ca_CheckboxDesc){.text="Clip children",.checked=p->clip,.on_change=clip_changed,.change_data=p,.id="property-clip"});
    field(p,BLUR,"Blur",true);
    if (filled) {field(p,GRADIENT_TO,"Gradient to",false);field(p,GRADIENT_ANGLE,"Gradient °",true);}
    if (shape||def->has_image||def->has_video) {
        field(p,SHADOW_BLUR,"Shadow blur",true);field(p,SHADOW_X,"Shadow X",true);field(p,SHADOW_Y,"Shadow Y",true);field(p,SHADOW_COLOR,"Shadow colour",false);
    }
    if (def->has_text) {
        static const char *const options[]={"Left","Center","Right"};
        ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW});
        ca_text(&(Ca_TextDesc){.text="Align",.style=FL_CLASS_PROPERTY_LABEL});
        ca_select(&(Ca_SelectDesc){.options=options,.option_count=FL_ALIGN_COUNT,.selected=(int)p->align,.id="property-align",
            .style=FL_CLASS_PROPERTY_SELECT,.on_change=align_chosen,.change_data=p});
        ca_div_end();
    }
    if (def->has_image||def->has_video) {
        field(p,CROP_X,"Crop X",true);field(p,CROP_Y,"Crop Y",true);field(p,CROP_W,"Crop width",true);field(p,CROP_H,"Crop height",true);
    }
    if (def->has_video) {
        field(p,VIDEO_START,"Start (s)",true);field(p,VIDEO_OFFSET,"Offset (s)",true);field(p,VIDEO_RATE,"Rate",true);
        ca_checkbox(&(Ca_CheckboxDesc){.text="Loop",.checked=p->loop,.on_change=loop_changed,.change_data=p,.id="property-loop"});
    }
}
/* Capture a boolean parameter in the same atomic Inspector draft. */
static void parameter_toggled(Ca_Checkbox *checkbox, void *data)
{
    Fl_ParameterField *field=data;
    if (field_set(&field->input,ca_checkbox_get(checkbox) ? "true" : "false")) {field->input.dirty=true; drafted(field->input.owner);}
}

/* Expose the same local-clock mapping available in component JSON and MCP. */
static void clock_controls(Fl_Inspector *p, const Fl_Scene *scene)
{
    ca_checkbox(&(Ca_CheckboxDesc){.text="Bind to beats",.checked=p->script_time.explicit_range,.on_change=clock_scope_changed,.change_data=p,.id="script-clock-scope"});
    if (p->script_time.explicit_range) {
        size_t count=scene->beat_count+1; const char **options=calloc(count,sizeof(*options)); Fl_Id *ids=calloc(count,sizeof(*ids));
        if (!options || !ids) {free(options); free(ids); return;}
        options[0]="Custom or missing anchor"; int start=0,end=0;
        for (size_t i=0;i<scene->beat_count;++i) {
            ids[i+1]=scene->beats[i].id; options[i+1]=scene->beats[i].name;
            if (p->script_time.start.kind==FL_ANCHOR_BEAT && p->script_time.start.beat==ids[i+1]) start=(int)i+1;
            if (p->script_time.end.kind==FL_ANCHOR_BEAT && p->script_time.end.beat==ids[i+1]) end=(int)i+1;
        }
        free(p->clock_beats); p->clock_beats=ids; p->clock_beat_count=count;
        ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW});
        ca_text(&(Ca_TextDesc){.text="Start beat",.style=FL_CLASS_PROPERTY_LABEL});
        ca_select(&(Ca_SelectDesc){.options=options,.option_count=(int)count,.selected=start,.id="script-clock-start",.style=FL_CLASS_PROPERTY_SELECT,.on_change=clock_start_chosen,.change_data=p}); ca_div_end();
        ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW});
        ca_text(&(Ca_TextDesc){.text="End beat",.style=FL_CLASS_PROPERTY_LABEL});
        ca_select(&(Ca_SelectDesc){.options=options,.option_count=(int)count,.selected=end,.id="script-clock-end",.style=FL_CLASS_PROPERTY_SELECT,.on_change=clock_end_chosen,.change_data=p}); ca_div_end();
        free(options);
    }
    static const char *const modes[]={"Stretch","Fixed speed"};
    ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW});
    ca_text(&(Ca_TextDesc){.text="Timing",.style=FL_CLASS_PROPERTY_LABEL});
    ca_select(&(Ca_SelectDesc){.options=modes,.option_count=2,.selected=p->script_time.mode,.id="script-clock-mode",.style=FL_CLASS_PROPERTY_SELECT,.on_change=clock_mode_chosen,.change_data=p}); ca_div_end();
    static const char *const labels[]={"Duration (s)","Rate","Start offset (s)","End offset (s)"}, *const ids[]={"script-clock-duration","script-clock-rate","script-clock-start-offset","script-clock-end-offset"};
    size_t fields=p->script_time.explicit_range ? 4 : 2;
    for (size_t i=0;i<fields;++i) {
        ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW}); ca_text(&(Ca_TextDesc){.text=labels[i],.style=FL_CLASS_PROPERTY_LABEL});
        ca_input(&(Ca_InputDesc){.text=p->clock_fields[i].value,.placeholder=i==0 ? "Auto" : NULL,.id=ids[i],.style=FL_CLASS_PROPERTY_INPUT,.on_change=edited,.change_data=&p->clock_fields[i]}); ca_div_end();
    }
    ca_checkbox(&(Ca_CheckboxDesc){.text="Loop",.checked=p->script_time.loop,.on_change=clock_loop_changed,.change_data=p,.id="script-clock-loop"});
    ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW});
    ca_checkbox(&(Ca_CheckboxDesc){.text="Hold before",.checked=p->script_time.before==FL_TIME_HOLD,.on_change=clock_before_changed,.change_data=p,.id="script-clock-before"});
    ca_checkbox(&(Ca_CheckboxDesc){.text="Hold after",.checked=p->script_time.after==FL_TIME_HOLD,.on_change=clock_after_changed,.change_data=p,.id="script-clock-after"}); ca_div_end();
}

/* Show which repository script the entity runs, whether it is enabled and the evaluator's current diagnostic. */
static void script_section(Fl_Inspector *p, const Fl_Scene *scene)
{
    ca_text(&(Ca_TextDesc){.text="Lua behaviors",.style=FL_CLASS_HEADING});
    const Fl_Entity *owner=fl_scene_entity(scene,p->entity);
    const char *attachments[65]; size_t attachment_count=0; int chosen=0;
    for (size_t i=0;owner && i<owner->component_count;++i) {
        const Fl_Component *component=&owner->components[i]; Fl_Script script;
        if (!fl_script_decode(component,&script)) continue;
        const Fl_Asset *asset=fl_project_asset(scene->project,script.script);
        p->script_instances[attachment_count]=component->id;
        snprintf(p->script_instance_labels[attachment_count],sizeof(p->script_instance_labels[0]),"%s · %llu",asset ? asset->name : "Missing Lua asset",(unsigned long long)component->id);
        attachments[attachment_count]=p->script_instance_labels[attachment_count];
        if (component->id==p->script_component) chosen=(int)attachment_count;
        ++attachment_count;
    }
    p->script_instances[attachment_count]=0; attachments[attachment_count]="New attachment";
    if (!p->script_component) chosen=(int)attachment_count;
    p->script_instance_count=attachment_count+1;
    ca_select(&(Ca_SelectDesc){.options=attachments,.option_count=(int)p->script_instance_count,.selected=chosen,.id="inspector-script-instance",
        .style=FL_CLASS_PROPERTY_SELECT,.on_change=script_instance_chosen,.change_data=p,.disabled=p->dirty});
    size_t total=fl_project_asset_count(scene->project), count=1;
    const char **options=calloc(total+1,sizeof(*options));
    Fl_Id *ids=calloc(total+1,sizeof(*ids));
    if (!options||!ids) {free(options);free(ids);return;}
    options[0]="None";int selected=0;
    for (size_t i=0;i<total;++i) {
        const Fl_Asset *asset=fl_project_asset_at(scene->project,i);
        if (asset->kind!=FL_ASSET_LUA) continue;
        ids[count]=asset->id;options[count]=asset->name;
        if (asset->id==p->script) selected=(int)count;
        ++count;
    }
    free(p->script_options);p->script_options=ids;p->script_option_count=count;
    ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW});
    ca_text(&(Ca_TextDesc){.text="Script",.style=FL_CLASS_PROPERTY_LABEL});
    ca_select(&(Ca_SelectDesc){.options=options,.option_count=(int)p->script_option_count,.selected=selected,.id="inspector-script",
        .style=FL_CLASS_PROPERTY_SELECT,.on_change=script_chosen,.change_data=p});
    ca_div_end();
    free(options);
    if (!p->script) return;
    clock_controls(p,scene);
    for (size_t i=0;i<p->parameter_count;++i) {
        Fl_ParameterField *field=&p->parameters[i]; char id[128];
        snprintf(id,sizeof(id),"script-param-%llu-%s",(unsigned long long)p->script_component,field->name);
        ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW});
        ca_text(&(Ca_TextDesc){.text=field->label,.style=FL_CLASS_PROPERTY_LABEL});
        if (!strcmp(field->type,"boolean")) ca_checkbox(&(Ca_CheckboxDesc){.checked=!strcmp(field->input.value,"true"),.on_change=parameter_toggled,.change_data=field,.id=id});
        else ca_input(&(Ca_InputDesc){.text=field->input.value,.id=id,.style=FL_CLASS_PROPERTY_INPUT,.on_change=edited,.change_data=&field->input});
        ca_div_end();
    }
    ca_checkbox(&(Ca_CheckboxDesc){.text="Enabled",.checked=p->script_enabled,.on_change=script_enabled,.change_data=p,.id="inspector-script-enabled"});
    const Fl_SceneState *state=fl_scene_edit_state(p->edit);
    const Fl_Entity *entity=fl_scene_entity(scene,p->entity);
    const char *error=state&&entity ? fl_script_runtime_component_error(fl_scene_state_scripts(state),p->script_component) : "";
    if (*error) ca_text(&(Ca_TextDesc){.text=error,.style=FL_CLASS_WARNING,.wrap=true});
    fl_button(&(Fl_ButtonDesc){.text="Edit script",.id="inspector-script-edit",.on_click=script_edit,.click_data=p,.disabled=p->dirty});
}

void fl_inspector_build(Ca_Dock *dock,Ca_Window *window,void *data)
{
    Fl_Inspector *p=data;p->dock=dock;p->window=window;
    const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(p->edit),fl_scene_edit_scene(p->edit));
    const Fl_Entity *entity=fl_scene_entity(scene,fl_scene_edit_primary(p->edit));
    p->selection_count=fl_scene_edit_count(p->edit);
    ca_div_begin(&(Ca_DivDesc){.direction=CA_VERTICAL,.style=FL_CLASS_INSPECTOR});
    if (!entity||fl_scene_edit_count(p->edit)!=1) {
        ca_text(&(Ca_TextDesc){.text=entity ? "Multiple selected" : "Nothing selected",.style=FL_CLASS_MUTED,.wrap=true});
        ca_div_end();return;
    }
    if ((p->refresh||p->entity!=entity->id||p->scene!=scene->id)&&!load(p,scene,entity)) {
        ca_text(&(Ca_TextDesc){.text="Could not load properties.",.style=FL_CLASS_MUTED});ca_div_end();return;
    }
    const Fl_WidgetDef *def=fl_widget_for_entity(entity);
    ca_text(&(Ca_TextDesc){.text=def ? def->name : "Entity",.style=FL_CLASS_HEADING});
    field(p,NAME,"Name",false);
    ca_checkbox(&(Ca_CheckboxDesc){.text="Visible",.checked=p->visible,.on_change=visibility_changed,.change_data=p});
    ca_text(&(Ca_TextDesc){.text="Transform",.style=FL_CLASS_HEADING});
    const char *labels[]={"X","Y","Width","Height","Rotation °","Scale X","Scale Y"};
    for (size_t i=0;i<7;++i) field(p,X+(int)i,labels[i],true);
    field(p,OPACITY,"Opacity",true);
    if (def&&(def->has_fill||def->has_stroke)) ca_text(&(Ca_TextDesc){.text="Appearance",.style=FL_CLASS_HEADING});
    if (def&&def->has_fill) field(p,FILL,"Fill",false);
    if (def&&def->has_stroke) {field(p,STROKE,"Stroke",false);field(p,STROKE_WIDTH,"Stroke px",true);}
    if (def&&def->has_text) {field(p,CONTENT,"Text",false);field(p,FONT_SIZE,"Font px",true);typography_section(p);}
    if (def&&def->has_image) image_section(p);
    style_section(p,def);
    const Fl_Animation *animation=fl_entity_animation(entity);
    if (animation) {
        char summary[48];snprintf(summary,sizeof(summary),"Keyframes: %zu track%s",animation->track_count,animation->track_count==1 ? "" : "s");
        ca_text(&(Ca_TextDesc){.text="Animation",.style=FL_CLASS_HEADING});
        ca_text(&(Ca_TextDesc){.text=summary,.style=FL_CLASS_MUTED});
    }
    script_section(p,scene);
    ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_TOOLBAR});
    fl_button(&(Fl_ButtonDesc){.text="Apply",.id="inspector-apply",.on_click=apply,.click_data=p});
    fl_button(&(Fl_ButtonDesc){.text="Revert",.id="inspector-revert",.on_click=revert,.click_data=p});
    ca_div_end();
    p->status=ca_text(&(Ca_TextDesc){.text=p->message,.style=FL_CLASS_MUTED,.wrap=true});
    ca_div_end();
}

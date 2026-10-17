#include "fl_inspector.h"
#include "style.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NAME,X,Y,WIDTH,HEIGHT,ROTATION,SCALE_X,SCALE_Y,FILL,STROKE,STROKE_WIDTH,FONT_SIZE,CONTENT,FONT_FILTER,FIELD_COUNT };
#define WEIGHT_OPTIONS 32
typedef struct Fl_InspectorField { struct Fl_Inspector *owner; char *value; } Fl_InspectorField;
typedef struct Fl_FontChoice { struct Fl_Inspector *owner; size_t family; uint64_t generation; } Fl_FontChoice;
struct Fl_Inspector {
    Fl_SceneEdit *edit;
    Fl_Fonts *fonts;
    Fl_EventBus *bus;
    Ca_Dock *dock;
    Ca_Label *status;
    Fl_Id scene,entity;
    uint64_t revision;
    size_t selection_count;
    Fl_InspectorField fields[FIELD_COUNT];
    Fl_Typography typography;
    Fl_Id image;
    Fl_FontChoice choices[FL_STYLE_FONT_LIST_LIMIT];
    unsigned weights[WEIGHT_OPTIONS];
    size_t weight_count;
    Fl_Id *image_options;
    size_t image_option_count;
    Fl_EventType project_event;
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
    if (field_set(field,ca_input_text(input))) {field->owner->dirty=true;status(field->owner,"Unsaved changes");}
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
static void apply(Ca_Button *button,void *data)
{
    (void)button;Fl_Inspector *p=data;
    const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(p->edit),p->scene);
    const Fl_Entity *entity=fl_scene_entity(scene,p->entity);
    if (!entity||scene->revision!=p->revision) {status(p,"Scene changed. Revert to reload before applying.");return;}
    double values[7];
    for (size_t i=0;i<7;++i) if (!number(p->fields[X+i].value,&values[i])) {status(p,"Enter finite numbers for every transform field.");return;}
    Fl_Transform transform={values[0],values[1],values[2],values[3],values[4]*3.14159265358979323846/180,values[5],values[6]};
    if (transform.width<0||transform.height<0) {status(p,"Width and height must be non-negative.");return;}
    const Fl_WidgetDef *def=fl_widget_for_entity(entity);Fl_Appearance appearance=fl_widget_appearance(entity);
    if ((def&&def->has_fill&&!color(p->fields[FILL].value,appearance.fill))||
        (def&&def->has_stroke&&(!color(p->fields[STROKE].value,appearance.stroke)||!number(p->fields[STROKE_WIDTH].value,&appearance.stroke_width)))||
        (def&&def->has_text&&!number(p->fields[FONT_SIZE].value,&appearance.font_size))) {status(p,"Check colour and appearance values.");return;}
    if (!p->fields[NAME].value||!p->fields[NAME].value[0]) {status(p,"Name cannot be empty.");return;}
    unsigned char bytes[56],style[24],typography[3+FL_TYPOGRAPHY_FAMILY_MAX],image[8];
    fl_transform_encode(&transform,bytes);fl_appearance_encode(&appearance,style);
    Fl_Component components[5]={{FL_COMPONENT_TRANSFORM,1,56,bytes}};size_t count=1;
    if (def&&def->kind!=FL_WIDGET_GROUP&&def->kind!=FL_WIDGET_IMAGE) components[count++]=(Fl_Component){FL_COMPONENT_APPEARANCE,1,24,style};
    if (def&&def->has_text) {
        size_t size=fl_typography_encode(&p->typography,typography);
        if (!size) {status(p,"Choose a font family.");return;}
        components[count++]=(Fl_Component){FL_COMPONENT_TEXT,1,strlen(p->fields[CONTENT].value),(const unsigned char *)p->fields[CONTENT].value};
        components[count++]=(Fl_Component){FL_COMPONENT_TYPOGRAPHY,1,size,typography};
    }
    if (def&&def->has_image) {
        if (!fl_project_asset(fl_scene_edit_project(p->edit),p->image)) {status(p,"Choose an image asset.");return;}
        fl_image_asset_encode(p->image,image);
        components[count++]=(Fl_Component){FL_COMPONENT_IMAGE,1,8,image};
    }
    p->applying=true;
    bool ok=fl_project_apply(fl_scene_edit_project(p->edit),&(Fl_Op){.kind=FL_OP_ENTITY_CONFIGURE,.scene=p->scene,.entity=p->entity,
        .name=p->fields[NAME].value,.visible=p->visible,.components=components,.component_count=count},NULL);
    p->applying=false;
    if (!ok) {status(p,"Invalid values or insufficient memory. Changes were not applied.");return;}
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

/* Snapshot current values into controller-owned strings; no document pointers survive a build. */
static bool load(Fl_Inspector *p,const Fl_Scene *scene,const Fl_Entity *entity)
{
    Fl_Transform transform={.scale_x=1,.scale_y=1};
    fl_transform_decode(fl_entity_component(entity,FL_COMPONENT_TRANSFORM),&transform);
    double values[7]={transform.x,transform.y,transform.width,transform.height,transform.rotation*180/3.14159265358979323846,transform.scale_x,transform.scale_y};
    if (!field_set(&p->fields[NAME],entity->name)) return false;
    char text[96];
    for (size_t i=0;i<7;++i) {snprintf(text,sizeof(text),"%.9g",values[i]);if (!field_set(&p->fields[X+i],text)) return false;}
    Fl_Appearance a=fl_widget_appearance(entity);
    snprintf(text,sizeof(text),"#%02X%02X%02X%02X",a.fill[0],a.fill[1],a.fill[2],a.fill[3]);if (!field_set(&p->fields[FILL],text)) return false;
    snprintf(text,sizeof(text),"#%02X%02X%02X%02X",a.stroke[0],a.stroke[1],a.stroke[2],a.stroke[3]);if (!field_set(&p->fields[STROKE],text)) return false;
    snprintf(text,sizeof(text),"%.9g",a.stroke_width);if (!field_set(&p->fields[STROKE_WIDTH],text)) return false;
    snprintf(text,sizeof(text),"%.9g",a.font_size);if (!field_set(&p->fields[FONT_SIZE],text)) return false;
    const Fl_Component *content=fl_entity_component(entity,FL_COMPONENT_TEXT);
    char *copy=malloc((content ? content->size : 0)+1);if (!copy) return false;
    if (content&&content->size) memcpy(copy,content->data,content->size);
    copy[content ? content->size : 0]=0;free(p->fields[CONTENT].value);p->fields[CONTENT].value=copy;
    p->typography=fl_widget_typography(entity);
    p->image=0;fl_image_asset_decode(fl_entity_component(entity,FL_COMPONENT_IMAGE),&p->image);
    p->visible=entity->visible;p->scene=scene->id;p->entity=entity->id;p->revision=scene->revision;p->refresh=false;return true;
}
/* Emit one labelled retained draft field with its stable callback binding. */
static void field(Fl_Inspector *p,int index,const char *label,bool numeric)
{
    ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_PROPERTY_ROW});
    ca_text(&(Ca_TextDesc){.text=label,.style=FL_CLASS_PROPERTY_LABEL});
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
Fl_Inspector *fl_inspector_create(Fl_SceneEdit *edit,Fl_Fonts *fonts,Fl_EventBus *bus)
{
    if (!edit||!fonts||!bus) return NULL;
    Fl_Inspector *p=calloc(1,sizeof(*p));if (!p) return NULL;
    p->edit=edit;p->fonts=fonts;p->bus=bus;p->refresh=true;
    for (int i=0;i<FIELD_COUNT;++i) p->fields[i].owner=p;
    for (size_t i=0;i<FL_STYLE_FONT_LIST_LIMIT;++i) p->choices[i].owner=p;
    p->project_event=fl_event_type_find(bus,FL_EVENT_PROJECT_CHANGED);
    const char *events[]={FL_EVENT_PROJECT_CHANGED,FL_EVENT_SELECTION_CHANGED};
    for (size_t i=0;i<2;++i) if (!fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=fl_event_type_find(bus,events[i]),
        .fn=changed,.user_data=p,.owner=p}).generation) {fl_inspector_destroy(p);return NULL;}
    if (!fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=fl_event_type_find(bus,FL_EVENT_FONTS_CHANGED),
        .fn=fonts_changed,.user_data=p,.owner=p}).generation) {fl_inspector_destroy(p);return NULL;}
    return p;
}
void fl_inspector_teardown(void *data) {Fl_Inspector *p=data;p->dock=NULL;p->status=NULL;}
void fl_inspector_destroy(Fl_Inspector *p)
{
    if (!p) return;
    fl_event_unsubscribe_owner(p->bus,p);for (int i=0;i<FIELD_COUNT;++i) free(p->fields[i].value);
    free(p->image_options);free(p);
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
    snprintf(line,sizeof(line),"Font: %s",p->typography.family);
    ca_text(&(Ca_TextDesc){.text=line,.style=FL_CLASS_LABEL,.wrap=true});
    if (!match.found) {
        snprintf(line,sizeof(line),"%s is not available on this computer; showing %s.",p->typography.family,FL_TYPOGRAPHY_DEFAULT_FAMILY);
        ca_text(&(Ca_TextDesc){.text=line,.style=FL_CLASS_WARNING,.wrap=true});
    }
    if (fl_fonts_scanning(p->fonts)) ca_text(&(Ca_TextDesc){.text="Loading installed fonts...",.style=FL_CLASS_MUTED});
    ca_input(&(Ca_InputDesc){.text=p->fields[FONT_FILTER].value,.placeholder="Search fonts",.id="property-font-filter",
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
        snprintf(line,sizeof(line),"Showing %zu of %zu families. Refine the search to see more.",shown,matching);
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
    if (match.synthetic_bold||match.synthetic_italic)
        ca_text(&(Ca_TextDesc){.text="This family has no matching style; it is synthesized.",.style=FL_CLASS_MUTED,.wrap=true});
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
void fl_inspector_build(Ca_Dock *dock,Ca_Window *window,void *data)
{
    (void)window;Fl_Inspector *p=data;p->dock=dock;
    const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(p->edit),fl_scene_edit_scene(p->edit));
    const Fl_Entity *entity=fl_scene_entity(scene,fl_scene_edit_primary(p->edit));
    p->selection_count=fl_scene_edit_count(p->edit);
    ca_div_begin(&(Ca_DivDesc){.direction=CA_VERTICAL,.style=FL_CLASS_INSPECTOR});
    if (!entity||fl_scene_edit_count(p->edit)!=1) {
        ca_text(&(Ca_TextDesc){.text=entity ? "Multiple entities selected. Drag in the viewport to move them together." : "Select an entity in the hierarchy or viewport.",.style=FL_CLASS_MUTED,.wrap=true});
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
    if (def&&(def->has_fill||def->has_stroke)) ca_text(&(Ca_TextDesc){.text="Appearance",.style=FL_CLASS_HEADING});
    if (def&&def->has_fill) field(p,FILL,"Fill",false);
    if (def&&def->has_stroke) {field(p,STROKE,"Stroke",false);field(p,STROKE_WIDTH,"Stroke px",true);}
    if (def&&def->has_text) {field(p,CONTENT,"Text",false);field(p,FONT_SIZE,"Font px",true);typography_section(p);}
    if (def&&def->has_image) image_section(p);
    ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_TOOLBAR});
    ca_btn_begin(&(Ca_BtnDesc){.text="Apply",.id="inspector-apply",.style=FL_CLASS_ACTION,.on_click=apply,.click_data=p});ca_btn_end();
    ca_btn_begin(&(Ca_BtnDesc){.text="Revert",.id="inspector-revert",.style=FL_CLASS_ACTION,.on_click=revert,.click_data=p});ca_btn_end();
    ca_div_end();
    p->status=ca_text(&(Ca_TextDesc){.text=p->message,.style=FL_CLASS_MUTED,.wrap=true});
    ca_div_end();
}

#include "fl_widget_library.h"
#include "style.h"
#include <ca_icons.h>
#include "core/fl_log.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fl_WidgetAction { struct Fl_WidgetLibrary *library; Fl_WidgetKind kind; } Fl_WidgetAction;
struct Fl_WidgetLibrary {
    Fl_SceneEdit *edit;
    Fl_ScenePanel *scene;
    Fl_EventBus *bus;
    Ca_Dock *dock;
    Fl_WidgetAction actions[FL_WIDGET_COUNT];
    char *query;
    Fl_Id active_scene;
};

/* Match registry names without allocating transformed strings. */
static bool matches(const char *name,const char *query)
{
    if (!query||!query[0]) return true;
    for (;*name;++name) {
        const char *a=name,*b=query;
        while (*a&&*b&&tolower((unsigned char)*a)==tolower((unsigned char)*b)) {++a;++b;}
        if (!*b) return true;
    }
    return false;
}
/* Insert by keyboard or pointer click at the scene centre. */
static void insert_centre(Fl_WidgetAction *action)
{
    Fl_SceneEdit *edit=action->library->edit;
    const Fl_Scene *scene=fl_project_scene(fl_scene_edit_project(edit),fl_scene_edit_scene(edit));
    const Fl_WidgetDef *def=fl_widget_definition(action->kind);
    if (scene&&!fl_scene_edit_insert(edit,action->kind,0,(scene->width-def->bounds.width)*.5,(scene->height-def->bounds.height)*.5))
        FL_LOG_ERROR("widgets","could not insert %s",def->name);
}
/* Pointer presses use the enclosing drag handler; keyboard activation inserts directly. */
static void activate(Ca_Button *button,void *data)
{
    float x,y;
    if (!ca_button_get_click_pos(button,&x,&y)) insert_centre(data);
}
/* Finish click-to-add or cross-window drag placement without retaining UI row pointers. */
static void drag_end(const Ca_DragEvent *event,void *data)
{
    Fl_WidgetAction *action=data;
    if (hypot(event->dx,event->dy)<FL_STYLE_WIDGET_DRAG_THRESHOLD) {insert_centre(action);return;}
    float x,y;
    if (ca_window_mouse_release_position(event->window,0,&x,&y)||ca_window_cursor_screen_position(event->window,&x,&y))
        fl_scene_panel_drop_widget(action->library->scene,action->kind,x,y);
}
/* Persist search text across reconciliation and floating-window remounts. */
static void search_changed(Ca_TextInput *input,void *data)
{
    Fl_WidgetLibrary *library=data;const char *text=ca_input_text(input);
    char *copy=malloc(strlen(text)+1);if (!copy) return;
    strcpy(copy,text);free(library->query);library->query=copy;
    if (library->dock) ca_dock_invalidate_panel(library->dock,"widgets");
}
/* Refresh insertion availability after scene creation/deletion or selection changes. */
static bool changed(const Fl_Event *event,void *data)
{
    (void)event;Fl_WidgetLibrary *library=data;
    if (library->active_scene!=fl_scene_edit_scene(library->edit)&&library->dock) ca_dock_invalidate_panel(library->dock,"widgets");
    return false;
}
Fl_WidgetLibrary *fl_widget_library_create(Fl_SceneEdit *edit,Fl_ScenePanel *scene,Fl_EventBus *bus)
{
    if (!edit||!scene||!bus) return NULL;
    Fl_WidgetLibrary *library=calloc(1,sizeof(*library));if (!library) return NULL;
    library->edit=edit;library->scene=scene;library->bus=bus;
    for (int i=0;i<FL_WIDGET_COUNT;++i) library->actions[i]=(Fl_WidgetAction){library,(Fl_WidgetKind)i};
    const char *events[]={FL_EVENT_PROJECT_CHANGED,FL_EVENT_SELECTION_CHANGED};
    for (size_t i=0;i<2;++i) if (!fl_event_subscribe(bus,&(Fl_SubscribeDesc){
        .type=fl_event_type_find(bus,events[i]),.fn=changed,.user_data=library,.owner=library}).generation) {
        fl_widget_library_destroy(library);return NULL;
    }
    return library;
}
void fl_widget_library_teardown(void *data) { Fl_WidgetLibrary *library=data;library->dock=NULL; }
void fl_widget_library_destroy(Fl_WidgetLibrary *library)
{
    if (!library) return;
    fl_event_unsubscribe_owner(library->bus,library);free(library->query);free(library);
}
void fl_widget_library_build(Ca_Dock *dock,Ca_Window *window,void *data)
{
    (void)window;Fl_WidgetLibrary *library=data;library->dock=dock;library->active_scene=fl_scene_edit_scene(library->edit);
    bool enabled=fl_project_scene(fl_scene_edit_project(library->edit),fl_scene_edit_scene(library->edit))!=NULL;
    ca_div_begin(&(Ca_DivDesc){.direction=CA_VERTICAL,.style=FL_CLASS_LIBRARY});
    ca_input(&(Ca_InputDesc){.text=library->query,.placeholder="Search",.id="widget-search",.style=FL_CLASS_INPUT,
        .on_change=search_changed,.change_data=library});
    static const char *icons[FL_WIDGET_COUNT]={CA_ICON_NF_FA_LAYER_GROUP,CA_ICON_NF_FA_SQUARE,CA_ICON_NF_FA_CIRCLE,
        CA_ICON_NF_FA_MINUS,CA_ICON_NF_FA_ARROW_RIGHT_LONG,CA_ICON_NF_FA_FONT,CA_ICON_NF_FA_IMAGE,CA_ICON_NF_FA_FILM,CA_ICON_NF_FA_CARET_UP,CA_ICON_NF_FA_DIAMOND,
        CA_ICON_NF_FA_DRAW_POLYGON,CA_ICON_NF_FA_STAR,CA_ICON_NF_FA_BEZIER_CURVE,CA_ICON_NF_FA_MAGIC};
    ca_div_begin(&(Ca_DivDesc){.direction=CA_HORIZONTAL,.style=FL_CLASS_ASSET_GRID});
    for (int kind=FL_WIDGET_ENTITY;kind<FL_WIDGET_COUNT;++kind) {
        if (kind==FL_WIDGET_IMAGE||kind==FL_WIDGET_VIDEO) continue;
        const Fl_WidgetDef *def=fl_widget_definition((Fl_WidgetKind)kind);
        if (!matches(def->name,library->query)) continue;
        Fl_WidgetAction *action=&library->actions[kind];
        ca_div_begin(&(Ca_DivDesc){.direction=CA_VERTICAL,.style=FL_CLASS_WIDGET_WRAP,.id=def->name,
            .on_drag_end=drag_end,.drag_data=action,.drag_preview=true,.disabled=!enabled});
        ca_btn_begin(&(Ca_BtnDesc){.direction=CA_VERTICAL,.style=FL_CLASS_WIDGET_CARD,.on_click=activate,.click_data=action,.disabled=!enabled});
        ca_text(&(Ca_TextDesc){.text=icons[kind],.style=FL_CLASS_WIDGET_ICON});
        ca_text(&(Ca_TextDesc){.text=def->name,.style=FL_CLASS_ASSET_NAME});
        ca_btn_end();ca_div_end();
        ca_tooltip(&(Ca_TooltipDesc){.text=def->description});
    }
    ca_div_end();
    ca_div_end();
}

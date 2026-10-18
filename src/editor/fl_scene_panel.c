#include "fl_scene_panel.h"
#include "components/fl_component.h"
#include "style.h"
#include "core/fl_log.h"
#include "fl_scene_preview.h"
#include "fl_canvas_view.h"
#include "fl_scene_thumbnails.h"
#include <ca_icons.h>
#include <ca_components.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fl_Row { struct Fl_ScenePanel *panel; Fl_Id scene, entity; Ca_Div *bounds; } Fl_Row;
struct Fl_ScenePanel {
    Fl_Project *project;
    Fl_SceneEdit *edit;
    Fl_EventBus *bus;
    Ca_Dock *dock;
    Ca_Window *window;
    Ca_Viewport *viewport;
    Ca_Div *hierarchy;
    Ca_Div *scene_strip;
    Ca_Effect *keys;
    bool gesturing, panning;
    double pan_dx, pan_dy;
    double drag_offset_x,drag_offset_y,drag_scale_x,drag_scale_y;
    Fl_Row *rows;
    size_t row_count;
    Fl_ScenePreview *preview;
    Fl_SceneThumbnails *thumbnails;
    Fl_Fonts *fonts;
    Ca_Window *rename_window;
    Ca_TextInput *rename_input;
    Fl_Id rename_scene, rename_entity;
    Fl_Id *collapsed;
    size_t collapsed_count, collapsed_capacity;
};

/* Invalidate the panel after a model event, retaining only ids still in the document. */
static bool changed(const Fl_Event *event, void *data)
{
    Fl_ScenePanel *p = data;
    const Fl_ProjectChange *change = event->payload;
    if (change->project != p->project) return false;
    if (p->dock) ca_dock_invalidate_panel(p->dock, "scene");
    return false;
}

/* Re-rasterize text when font catalogs change (system scan finished, project fonts imported). */
static bool fonts_changed(const Fl_Event *event, void *data)
{
    Fl_ScenePanel *p = data;
    const Fl_FontsChanged *change = event->payload;
    if (change->fonts == p->fonts && p->viewport) ca_viewport_request_redraw(p->viewport);
    return false;
}

/* Redraw the viewport at the new scene time. */
static bool time_changed(const Fl_Event *event, void *data)
{
    Fl_ScenePanel *p = data;
    const Fl_TimeChange *change = event->payload;
    if (change->edit == p->edit && p->viewport) ca_viewport_request_redraw(p->viewport);
    return false;
}

/* Preview callback: redraw once asynchronously decoded images are ready to upload. */
static void preview_invalidated(void *data)
{
    Fl_ScenePanel *p = data;
    if (p->viewport) ca_viewport_request_redraw(p->viewport);
}

/* Reflect shared selection changes in the hierarchy, cards and viewport. */
static bool selection_changed(const Fl_Event *event, void *data)
{
    Fl_ScenePanel *p=data;const Fl_SelectionChange *change=event->payload;
    if (change->edit==p->edit&&p->dock) ca_dock_invalidate_panel(p->dock,"scene");
    return false;
}

/* Apply a model operation and expose failures in the existing Console. */
static bool apply(Fl_ScenePanel *p, Fl_Op op, Fl_Id *id)
{
    bool ok = fl_project_apply(p->project, &op, id);
    if (!ok) FL_LOG_ERROR("scene", "operation rejected or allocation failed");
    return ok;
}

typedef struct Fl_AspectPreset { const char *name; uint64_t width, height; } Fl_AspectPreset;
static const Fl_AspectPreset aspect_presets[] = {
    {"Canvas", 0, 0}, {"16:9", 16, 9}, {"9:16", 9, 16}, {"1:1", 1, 1},
    {"4:3", 4, 3}, {"3:4", 3, 4}, {"21:9", 21, 9}
};

/* Apply the selected target ratio to the active scene without changing its canvas or entities. */
static void aspect_chosen(Ca_Select *select, void *data)
{
    Fl_ScenePanel *p = data;
    int index = ca_select_get(select);
    if (index < 0 || (size_t)index >= sizeof(aspect_presets) / sizeof(*aspect_presets)) return;
    const Fl_AspectPreset *preset = &aspect_presets[index];
    apply(p, (Fl_Op){.kind = FL_OP_SCENE_ASPECT, .scene = fl_scene_edit_scene(p->edit),
        .aspect_width = preset->width, .aspect_height = preset->height}, NULL);
}

/* Toggle authoring snap independently of document contents. */
static void toggle_snap(Ca_Button *button, void *data)
{
    (void)button; Fl_ScenePanel *p=data;
    fl_scene_edit_set_snap(p->edit,!fl_scene_edit_snap(p->edit));
}

/* Reset the saved camera to the canvas center. */
static void reset_canvas(Ca_Button *button, void *data)
{
    (void)button; Fl_ScenePanel *p=data; fl_scene_preview_reset_view(p->preview);
}

/* Canvas context actions share the toolbar camera and snapping owners. */
static void canvas_menu(int index, void *data)
{
    if (index==0) reset_canvas(NULL,data);
    else if (index==1) toggle_snap(NULL,data);
}

/* Refresh the toolbar after an authoring snap preference changes. */
static bool snap_changed(const Fl_Event *event, void *data)
{
    Fl_ScenePanel *p=data; const Fl_SnapChange *change=event->payload;
    if (change->edit==p->edit&&p->dock) ca_dock_invalidate_panel(p->dock,"scene");
    return false;
}

/* Emit a right-aligned target selector; unlisted stored ratios remain visible as Custom. */
static void build_aspect_selector(Fl_ScenePanel *p, const Fl_Scene *scene)
{
    const char *options[sizeof(aspect_presets) / sizeof(*aspect_presets) + 1];
    int count = (int)(sizeof(aspect_presets) / sizeof(*aspect_presets)), selected = -1;
    for (int i = 0; i < count; ++i) {
        options[i] = aspect_presets[i].name;
        if (scene && ((!scene->aspect_width && i == 0) ||
            (scene->aspect_width && aspect_presets[i].width &&
             scene->aspect_width * aspect_presets[i].height == scene->aspect_height * aspect_presets[i].width))) selected = i;
    }
    char custom[64];
    if (scene && selected < 0) {
        snprintf(custom, sizeof(custom), "%llu:%llu", (unsigned long long)scene->aspect_width,
            (unsigned long long)scene->aspect_height);
        selected = count; options[count++] = custom;
    }
    ca_div_begin(&(Ca_DivDesc){.style = FL_CLASS_TOOLBAR_SPACER}); ca_div_end();
    ca_btn_begin(&(Ca_BtnDesc){.text=CA_ICON_NF_MD_FIT_TO_PAGE_OUTLINE,.id="scene-fit-canvas",
        .style=FL_CLASS_CANVAS_TOOL,.on_click=reset_canvas,.click_data=p}); ca_btn_end();
    ca_tooltip(&(Ca_TooltipDesc){.text="Center target frame. Pan with right-drag or scroll."});
    ca_btn_begin(&(Ca_BtnDesc){.text=CA_ICON_NF_MD_MAGNET,.id="scene-toggle-snap",
        .style=fl_scene_edit_snap(p->edit) ? FL_CLASS_CANVAS_TOOL_ACTIVE : FL_CLASS_CANVAS_TOOL,
        .on_click=toggle_snap,.click_data=p}); ca_btn_end();
    ca_tooltip(&(Ca_TooltipDesc){.text=fl_scene_edit_snap(p->edit) ? "Grid snapping on (64 units)" : "Grid snapping off"});
    ca_text(&(Ca_TextDesc){.text = "Target", .style = FL_CLASS_LABEL});
    ca_select(&(Ca_SelectDesc){.options = options, .option_count = count, .selected = selected < 0 ? 0 : selected,
        .id = "scene-target-aspect", .style = FL_CLASS_ASPECT_SELECT, .disabled = !scene,
        .on_change = aspect_chosen, .change_data = p});
}

/* Select a card or hierarchy row by stable identity. */
static void select_row(Ca_Button *button, void *data)
{
    (void)button; Fl_Row row = *(Fl_Row *)data;
    bool additive=row.entity&&row.panel->window&&(ca_window_mouse_mods(row.panel->window)&CA_MOD_SHIFT);
    fl_scene_edit_select(row.panel->edit,row.scene,row.entity,additive);
}

/* Create a scene at the end of the project's render order. */
static void add_scene(Ca_Button *button, void *data)
{
    (void)button; Fl_ScenePanel *p = data; Fl_Id id;
    char name[64]; snprintf(name, sizeof(name), "Scene %zu", fl_project_scene_count(p->project) + 1);
    if (apply(p, (Fl_Op){.kind = FL_OP_SCENE_ADD, .name = name}, &id)) fl_scene_edit_select(p->edit,id,0,false);
}

/* Add a group or rectangle under parent, then select its stable identity. */
static void add_entity(Fl_ScenePanel *p, Fl_Id scene, Fl_Id parent, bool rectangle)
{
    Fl_Id id;
    if (fl_widget_create(p->project,scene,parent,rectangle ? FL_WIDGET_RECTANGLE : FL_WIDGET_GROUP,0,0,&id))
        fl_scene_edit_select(p->edit,scene,id,false);
}

/* Release borrowed rename handles when its separate utility window closes. */
static void rename_closed(Ca_Window *window, void *data)
{
    (void)window; Fl_ScenePanel *p = data; p->rename_window = NULL; p->rename_input = NULL;
}

/* Commit a rename using ids captured when the utility window opened. */
static void rename_apply(Ca_Button *button, void *data)
{
    (void)button; Fl_ScenePanel *p = data;
    if (apply(p, (Fl_Op){.kind = p->rename_entity ? FL_OP_ENTITY_RENAME : FL_OP_SCENE_RENAME,
                         .scene = p->rename_scene, .entity = p->rename_entity,
                         .name = ca_input_text(p->rename_input)}, NULL)) ca_window_close(p->rename_window);
}

/* Open a focused rename form, retaining no pointers into mutable document arrays. */
static void rename_row(Fl_Row row)
{
    Fl_ScenePanel *p = row.panel;
    if (p->rename_window) { ca_window_destroy(p->rename_window); }
    const Fl_Scene *s = fl_project_scene(p->project, row.scene);
    const Fl_Entity *e = fl_scene_entity(s, row.entity);
    if (!s || (row.entity && !e)) return;
    p->rename_scene = row.scene; p->rename_entity = row.entity;
    p->rename_window = ca_window_create(ca_window_instance(p->window), &(Ca_WindowDesc){
        .title = "Rename", .width = FL_STYLE_FORM_W, .height = FL_STYLE_RENAME_H,
        .on_close = rename_closed, .on_close_data = p,
    });
    if (!p->rename_window) return;
    ca_ui_begin(p->rename_window, &(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_FORM});
    p->rename_input = ca_input(&(Ca_InputDesc){.text = e ? e->name : s->name, .style = FL_CLASS_INPUT});
    ca_btn_begin(&(Ca_BtnDesc){.text = "Rename", .on_click = rename_apply, .click_data = p, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_ui_end(); ca_input_focus(p->rename_input);
}

/* Scene card actions operate on the clicked card rather than global selection. */
static void scene_menu(int index, void *data)
{
    Fl_Row row = *(Fl_Row *)data; Fl_ScenePanel *p = row.panel;
    if (index == 0) { rename_row(row); return; }
    size_t count = fl_project_scene_count(p->project), at = 0;
    while (at < count && fl_project_scene_at(p->project, at)->id != row.scene) ++at;
    if (at == count) return;
    if (index == 3) apply(p, (Fl_Op){.kind = FL_OP_SCENE_REMOVE, .scene = row.scene}, NULL);
    else if ((index == 1 && at) || (index == 2 && at + 1 < count))
        apply(p, (Fl_Op){.kind = FL_OP_SCENE_MOVE, .scene = row.scene, .position = index == 1 ? at - 1 : at + 1}, NULL);
}

/* Reorder a scene card on release inside the strip, preserving stable scene identity. */
static void scene_drag_end(const Ca_DragEvent *event, void *data)
{
    Fl_Row row = *(Fl_Row *)data;
    Fl_ScenePanel *p = row.panel;
    if (hypotf(event->dx, event->dy) < FL_STYLE_WIDGET_DRAG_THRESHOLD * ca_instance_get_scale(ca_window_instance(event->window)) || !p->scene_strip) return;
    float x, y, w, h;
    ca_div_content_screen_rect(p->scene_strip, &x, &y, &w, &h);
    if (event->x < x || event->x > x + w || event->y < y || event->y > y + h) return;
    size_t count = fl_project_scene_count(p->project), from = count, to = 0;
    for (size_t i = 0; i < count; ++i) {
        if (fl_project_scene_at(p->project, i)->id == row.scene) { from = i; continue; }
        ca_div_screen_rect(p->rows[i].bounds, &x, NULL, &w, NULL);
        if (event->x >= x + w * .5f) ++to;
    }
    if (from < count && from != to)
        apply(p, (Fl_Op){.kind = FL_OP_SCENE_MOVE, .scene = row.scene, .position = to}, NULL);
}

/* Context actions create children, change parent, reorder siblings, or delete a subtree. */
static void entity_menu(int index, void *data)
{
    Fl_Row row = *(Fl_Row *)data; Fl_ScenePanel *p = row.panel;
    const Fl_Scene *s = fl_project_scene(p->project, row.scene);
    const Fl_Entity *e = fl_scene_entity(s, row.entity);
    if (!e) return;
    if (index == 0) rename_row(row);
    else if (index == 1 || index == 2) add_entity(p, row.scene, row.entity, index == 2);
    else if (index == 3) apply(p, (Fl_Op){.kind = FL_OP_ENTITY_VISIBLE, .scene = row.scene, .entity = row.entity, .visible = !e->visible}, NULL);
    else if (index == 4) apply(p, (Fl_Op){.kind = FL_OP_ENTITY_PARENT, .scene = row.scene, .entity = row.entity}, NULL);
    else if (index == 7) apply(p, (Fl_Op){.kind = FL_OP_ENTITY_REMOVE, .scene = row.scene, .entity = row.entity}, NULL);
    else if (index == 8 && fl_scene_edit_primary(p->edit) && fl_scene_edit_primary(p->edit) != e->id)
        apply(p, (Fl_Op){.kind = FL_OP_ENTITY_PARENT, .scene = row.scene, .entity = row.entity, .parent = fl_scene_edit_primary(p->edit)}, NULL);
    else if (index == 5 || index == 6) {
        size_t at = (size_t)(e - s->entities), to = at;
        if (index == 5) { while (to) { --to; if (s->entities[to].parent == e->parent) break; } }
        else { while (++to < s->entity_count) if (s->entities[to].parent == e->parent) break; }
        if (to < s->entity_count && s->entities[to].parent == e->parent)
            apply(p, (Fl_Op){.kind = FL_OP_ENTITY_MOVE, .scene = row.scene, .entity = row.entity, .position = to}, NULL);
    }
}

/* Start selection/manipulation from viewport pixels, preserving Shift selection. */
static void stage_start(const Ca_DragEvent *event,void *data)
{
    Fl_ScenePanel *p=data;double x,y;
    if (p->viewport && event->button==1) {
        p->panning=true; p->pan_dx=p->pan_dy=0; return;
    }
    if (!p->viewport||!fl_scene_preview_point(p->preview,p->viewport,event->start_x,event->start_y,true,&x,&y)) return;
    double zero_x,zero_y,unit_x,unit_y;
    if (!fl_scene_preview_point(p->preview,p->viewport,0,0,true,&zero_x,&zero_y)||
        !fl_scene_preview_point(p->preview,p->viewport,1,1,true,&unit_x,&unit_y)) return;
    p->drag_offset_x=zero_x;p->drag_offset_y=zero_y;
    p->drag_scale_x=unit_x-zero_x;p->drag_scale_y=unit_y-zero_y;
    Fl_EditGesture mode=fl_scene_preview_handle(p->preview,p->viewport,x,y);
    if (mode==FL_GESTURE_MOVE) {
        if (!fl_scene_preview_point(p->preview,p->viewport,event->start_x,event->start_y,false,&x,&y)) return;
        const Fl_Scene *s=fl_project_scene(p->project,fl_scene_edit_scene(p->edit));
        Fl_Id hit=fl_scene_geometry_pick(fl_scene_edit_geometry(p->edit),s,x,y,5);
        bool additive=(ca_window_mouse_mods(event->window)&CA_MOD_SHIFT)!=0;
        if (additive||!fl_scene_edit_contains(p->edit,hit)) fl_scene_edit_select(p->edit,s->id,hit,additive);
        if (!hit||!fl_scene_edit_contains(p->edit,hit)) return;
    }
    p->gesturing=fl_scene_edit_begin(p->edit,mode,x,y);
}
/* Continue a captured gesture even beyond the artboard's bounds. */
static void stage_move(const Ca_DragEvent *event,void *data)
{
    Fl_ScenePanel *p=data;
    if (p->panning) {
        if (p->viewport) fl_scene_preview_pan(p->preview,p->viewport,event->dx-p->pan_dx,event->dy-p->pan_dy);
        p->pan_dx=event->dx; p->pan_dy=event->dy; return;
    }
    if (p->gesturing) fl_scene_edit_update(p->edit,event->x*p->drag_scale_x+p->drag_offset_x,
        event->y*p->drag_scale_y+p->drag_offset_y);
}
/* Finish pointer editing; project persistence observes the resulting operations. */
static void stage_end(const Ca_DragEvent *event,void *data)
{
    Fl_ScenePanel *p=data;
    if (p->panning) {stage_move(event,p);p->panning=false;return;}
    if (!p->gesturing) return;
    stage_move(event,p);fl_scene_edit_end(p->edit,false);p->gesturing=false;
}
/* Scroll and trackpad deltas pan the editor camera; object drags keep their captured mapping. */
static void stage_scroll(double dx, double dy, void *data)
{
    Fl_ScenePanel *p=data;
    if (p->viewport&&!p->gesturing&&!p->panning) fl_scene_preview_pan(p->preview,p->viewport,dx*32,dy*32);
}

/* Escape rolls back only the active viewport gesture. */
static void check_keys(void *data)
{
    Fl_ScenePanel *p=data;
    if (p->panning&&p->window&&ca_window_focused(p->window)&&ca_window_key_down(p->window,CA_KEY_ESCAPE)) {
        if (p->viewport) fl_scene_preview_pan(p->preview,p->viewport,-p->pan_dx,-p->pan_dy);
        p->panning=false;
    }
    if (p->gesturing&&p->window&&ca_window_focused(p->window)&&ca_window_key_down(p->window,CA_KEY_ESCAPE)) {
        fl_scene_edit_end(p->edit,true);p->gesturing=false;
    }
}

Fl_ScenePanel *fl_scene_panel_create(Fl_SceneEdit *edit, Fl_Fonts *fonts, Fl_JobSystem *jobs, Fl_EventBus *bus)
{
    Fl_Project *project=fl_scene_edit_project(edit);
    if (!project || !fonts || !jobs || !bus) return NULL;
    Fl_ScenePanel *p = calloc(1, sizeof(*p)); if (!p) return NULL;
    p->project = project; p->bus = bus; p->edit=edit; p->fonts=fonts;
    p->preview = fl_scene_preview_create(&(Fl_ScenePreviewDesc){.edit=edit,.fonts=fonts,.jobs=jobs,.bus=bus,
        .invalidate=preview_invalidated,.invalidate_data=p});
    if (!p->preview) { free(p); return NULL; }
    Fl_EventType type = fl_event_type_register(bus, FL_EVENT_PROJECT_CHANGED, sizeof(Fl_ProjectChange));
    Fl_EventType selection=fl_event_type_find(bus,FL_EVENT_SELECTION_CHANGED);
    Fl_EventType fonts_event=fl_event_type_find(bus,FL_EVENT_FONTS_CHANGED);
    if (!fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=fl_event_type_find(bus,FL_EVENT_SNAP_CHANGED),.fn=snap_changed,.user_data=p,.owner=p}).generation ||
        !fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=selection,.fn=selection_changed,.user_data=p,.owner=p}).generation ||
        !fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=fonts_event,.fn=fonts_changed,.user_data=p,.owner=p}).generation ||
        !fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=fl_event_type_find(bus,FL_EVENT_TIME_CHANGED),.fn=time_changed,.user_data=p,.owner=p}).generation ||
        type == FL_EVENT_TYPE_INVALID || !fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = type, .fn = changed, .user_data = p, .owner = p}).generation) {
        fl_event_unsubscribe_owner(bus,p); fl_scene_preview_destroy(p->preview); free(p); return NULL;
    }
    return p;
}

void fl_scene_panel_teardown(void *data)
{
    Fl_ScenePanel *p = data; p->dock = NULL; p->window = NULL; p->viewport=NULL; p->hierarchy=NULL;p->scene_strip=NULL;
}

void fl_scene_panel_destroy(Fl_ScenePanel *p)
{
    if (!p) return;
    if (p->rename_window) ca_window_destroy(p->rename_window);
    fl_scene_panel_teardown(p); fl_event_unsubscribe_owner(p->bus, p);
    ca_effect_destroy(p->keys);
    fl_scene_preview_destroy(p->preview);
    fl_scene_thumbnails_destroy(p->thumbnails);
    free(p->collapsed); free(p->rows); free(p);
}

/* Position of an entity in the collapsed set, or the set's size when it is expanded. */
static size_t collapsed_index(const Fl_ScenePanel *p, Fl_Id id)
{
    size_t i = 0;
    while (i < p->collapsed_count && p->collapsed[i] != id) ++i;
    return i;
}

/* Flip an entity between collapsed and expanded; false when the set cannot grow. */
static bool toggle_collapsed(Fl_ScenePanel *p, Fl_Id id)
{
    size_t at = collapsed_index(p, id);
    if (at < p->collapsed_count) {
        p->collapsed[at] = p->collapsed[--p->collapsed_count];
        return true;
    }
    if (p->collapsed_count == p->collapsed_capacity) {
        size_t capacity = p->collapsed_capacity ? p->collapsed_capacity * 2 : 16;
        if (capacity > SIZE_MAX / sizeof(*p->collapsed)) return false;
        Fl_Id *grown = realloc(p->collapsed, capacity * sizeof(*grown));
        if (!grown) return false;
        p->collapsed = grown; p->collapsed_capacity = capacity;
    }
    p->collapsed[p->collapsed_count++] = id;
    return true;
}

/* Forget collapsed entities that left the scene, and expand the ancestors of the primary selection so it stays visible. */
static void sync_collapsed(Fl_ScenePanel *p, const Fl_Scene *s)
{
    for (size_t i = 0; i < p->collapsed_count; ) {
        if (fl_scene_entity(s, p->collapsed[i])) ++i;
        else p->collapsed[i] = p->collapsed[--p->collapsed_count];
    }
    const Fl_Entity *e = fl_scene_entity(s, fl_scene_edit_primary(p->edit));
    for (size_t hops = 0; e && e->parent && hops <= s->entity_count; ++hops) {
        size_t at = collapsed_index(p, e->parent);
        if (at < p->collapsed_count) p->collapsed[at] = p->collapsed[--p->collapsed_count];
        e = fl_scene_entity(s, e->parent);
    }
}

/* Expand or collapse a hierarchy row's children. */
static void toggle_expand(Ca_Button *button, void *data)
{
    (void)button; Fl_Row row = *(Fl_Row *)data;
    if (toggle_collapsed(row.panel, row.entity) && row.panel->dock) ca_dock_invalidate_panel(row.panel->dock, "scene");
}

/* True when `descendant` lies below `ancestor`. */
static bool is_below(const Fl_Scene *s, const Fl_Entity *descendant, Fl_Id ancestor)
{
    for (size_t hops = 0; descendant && descendant->parent && hops <= s->entity_count; ++hops) {
        if (descendant->parent == ancestor) return true;
        descendant = fl_scene_entity(s, descendant->parent);
    }
    return false;
}

/* Eye button: hide a shown entity, show a hidden one, and reveal everything under a mixed group. */
static void toggle_visibility(Ca_Button *button, void *data)
{
    (void)button; Fl_Row row = *(Fl_Row *)data; Fl_ScenePanel *p = row.panel;
    const Fl_Scene *s = fl_project_scene(p->project, row.scene);
    const Fl_Entity *e = fl_scene_entity(s, row.entity);
    if (!e) return;
    bool mixed = false;
    for (size_t i = 0; e->visible && i < s->entity_count && !mixed; ++i)
        mixed = !s->entities[i].visible && is_below(s, &s->entities[i], e->id);
    if (!mixed) {
        apply(p, (Fl_Op){.kind = FL_OP_ENTITY_VISIBLE, .scene = row.scene, .entity = row.entity, .visible = !e->visible}, NULL);
        return;
    }
    for (size_t i = 0; i < s->entity_count; ++i) {
        const Fl_Entity *below = &s->entities[i];
        if (!below->visible && is_below(s, below, e->id))
            apply(p, (Fl_Op){.kind = FL_OP_ENTITY_VISIBLE, .scene = row.scene, .entity = below->id, .visible = true}, NULL);
    }
}

/* Icon naming an entity's primitive. */
static const char *kind_icon(const Fl_Entity *e, bool open)
{
    const Fl_WidgetDef *def = fl_widget_for_entity(e);
    switch (def ? def->kind : FL_WIDGET_GROUP) {
    case FL_WIDGET_RECTANGLE: return CA_ICON_NF_FA_SQUARE;
    case FL_WIDGET_ELLIPSE: return CA_ICON_NF_FA_CIRCLE;
    case FL_WIDGET_TEXT: return CA_ICON_NF_FA_FONT;
    case FL_WIDGET_IMAGE: return CA_ICON_NF_FA_IMAGE;
    case FL_WIDGET_LINE: case FL_WIDGET_ARROW: return CA_ICON_NF_FA_ARROW_RIGHT;
    default: return open ? CA_ICON_NF_FA_FOLDER_OPEN : CA_ICON_NF_FA_FOLDER;
    }
}

#define LINK_NONE SIZE_MAX
#define ENTITY_HIDDEN_ABOVE 1u   /* the entity or an ancestor is hidden */
#define ENTITY_HIDDEN_BELOW 2u   /* some descendant is hidden */

/* Emit the guide cells left of a row: a continuing line for every ancestor with later siblings, then this row's elbow. */
static void emit_guides(size_t depth, const size_t *chain, const size_t *next, size_t self)
{
    for (size_t level = 1; level < depth; ++level) {
        ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TREE_GUIDE, .no_hover = true});
        if (next[chain[level]] != LINK_NONE) {
            ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TREE_LINE, .no_hover = true}); ca_div_end();
        }
        ca_div_end();
    }
    if (!depth) return;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TREE_GUIDE, .no_hover = true});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TREE_ELBOW_TOP, .no_hover = true}); ca_div_end();
    if (next[self] != LINK_NONE) {
        ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TREE_ELBOW_BOTTOM, .no_hover = true}); ca_div_end();
    }
    ca_div_end();
}

/* Build the entity tree of a scene: guide lines, disclosure chevrons, kind icons and per-row visibility buttons. */
static void build_hierarchy(Fl_ScenePanel *p, const Fl_Scene *s)
{
    const size_t n = s ? s->entity_count : 0;
    if (!n) {
        ca_text(&(Ca_TextDesc){.text = s ? "Add an entity to begin." : "Create a scene to begin.", .style = FL_CLASS_MUTED});
        return;
    }
    sync_collapsed(p, s);
    size_t *links = n <= SIZE_MAX / sizeof(size_t) / 6 ? malloc((n * 6 + 1) * sizeof(size_t)) : NULL;
    unsigned char *flags = calloc(n, 1);
    if (!links || !flags) { free(links); free(flags); return; }
    size_t *first = links, *next = links + n, *parent = links + n * 2, *order = links + n * 3,
           *depth = links + n * 4, *chain = links + n * 5;
    for (size_t i = 0; i < n; ++i) first[i] = next[i] = parent[i] = LINK_NONE;
    size_t first_root = LINK_NONE;
    for (size_t i = n; i > 0; ) {
        --i; const Fl_Entity *above = fl_scene_entity(s, s->entities[i].parent);
        parent[i] = above ? (size_t)(above - s->entities) : LINK_NONE;
        size_t *head = above ? &first[parent[i]] : &first_root;
        next[i] = *head; *head = i;
    }
    size_t count = 0, level = 0;
    for (size_t i = first_root; i != LINK_NONE; ) {
        order[count++] = i; depth[i] = level;
        if (first[i] != LINK_NONE) { chain[level++] = i; i = first[i]; }
        else {
            while (next[i] == LINK_NONE && level) i = chain[--level];
            i = next[i];
        }
    }
    for (size_t k = 0; k < count; ++k) {
        const size_t i = order[k];
        if (!s->entities[i].visible || (parent[i] != LINK_NONE && (flags[parent[i]] & ENTITY_HIDDEN_ABOVE)))
            flags[i] |= ENTITY_HIDDEN_ABOVE;
    }
    for (size_t k = count; k > 0; ) {
        const size_t i = order[--k];
        if (parent[i] != LINK_NONE && (!s->entities[i].visible || (flags[i] & ENTITY_HIDDEN_BELOW)))
            flags[parent[i]] |= ENTITY_HIDDEN_BELOW;
    }
    size_t skip_below = LINK_NONE;
    for (size_t k = 0; k < count; ++k) {
        const size_t i = order[k], d = depth[i];
        if (skip_below != LINK_NONE) {
            if (d > skip_below) continue;
            skip_below = LINK_NONE;
        }
        chain[d] = i;
        const Fl_Entity *e = &s->entities[i];
        const bool branch = first[i] != LINK_NONE || e->component_count != 0, collapsed = branch && collapsed_index(p, e->id) < p->collapsed_count;
        Fl_Row *row = &p->rows[p->row_count++]; *row = (Fl_Row){.panel=p,.scene=s->id,.entity=e->id};
        char id[48]; snprintf(id, sizeof(id), "entity-%llu", (unsigned long long)e->id);
        row->bounds = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_ENTITY_ROW, .id = id});
        ca_btn_begin(&(Ca_BtnDesc){.on_click = select_row, .click_data = row, .id = id,
            .style = fl_scene_edit_contains(p->edit, e->id) ? FL_CLASS_TREE_ROW_SELECTED : FL_CLASS_TREE_ROW});
        emit_guides(d, chain, next, i);
        if (branch) {
            char key[56]; snprintf(key, sizeof(key), "%s-open", id);
            ca_btn_begin(&(Ca_BtnDesc){.text = collapsed ? CA_ICON_NF_FA_CHEVRON_RIGHT : CA_ICON_NF_FA_CHEVRON_DOWN,
                .on_click = toggle_expand, .click_data = row, .id = key, .style = FL_CLASS_TREE_CHEVRON});
            ca_btn_end();
        } else {
            ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TREE_LEAF, .no_hover = true}); ca_div_end();
        }
        ca_text(&(Ca_TextDesc){.text = kind_icon(e, branch && !collapsed), .style = FL_CLASS_TREE_ICON});
        ca_text(&(Ca_TextDesc){.text = e->name, .style = (flags[i] & ENTITY_HIDDEN_ABOVE) ? FL_CLASS_TREE_NAME_MUTED : FL_CLASS_TREE_NAME});
        const char *eye_style = !e->visible ? FL_CLASS_TREE_EYE_HIDDEN : (flags[i] & ENTITY_HIDDEN_BELOW) ? FL_CLASS_TREE_EYE_MIXED : FL_CLASS_TREE_EYE;
        char key[56]; snprintf(key, sizeof(key), "%s-eye", id);
        ca_btn_begin(&(Ca_BtnDesc){.text = e->visible ? CA_ICON_NF_FA_EYE : CA_ICON_NF_FA_EYE_SLASH,
            .on_click = toggle_visibility, .click_data = row, .id = key, .style = eye_style});
        ca_btn_end();
        ca_btn_end();
        const char *items[] = {"Rename", "Add child group", "Add child rectangle", "Toggle visibility", "Move to root", "Move up", "Move down", "Delete subtree", "Parent to selected"};
        ca_context_menu(&(Ca_CtxMenuDesc){.items = items, .item_count = 9, .on_select = entity_menu, .select_data = row});
        if (!collapsed) for (size_t component = 0; component < e->component_count; ++component) {
            const Fl_Component *c = &e->components[component];
            const char *name = fl_component_name(c->type);
            char label[64], component_id[80];
            if (!name) {snprintf(label, sizeof(label), "Component %llu", (unsigned long long)c->type); name = label;}
            snprintf(component_id, sizeof(component_id), "%s-component-%llu", id, (unsigned long long)c->type);
            ca_btn_begin(&(Ca_BtnDesc){.on_click = select_row, .click_data = row, .id = component_id, .style = FL_CLASS_TREE_ROW});
            for (size_t indent = 0; indent <= d; ++indent) {
                ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TREE_GUIDE, .no_hover = true}); ca_div_end();
            }
            ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_TREE_LEAF, .no_hover = true}); ca_div_end();
            ca_text(&(Ca_TextDesc){.text = name, .style = FL_CLASS_TREE_NAME_MUTED});
            ca_btn_end();
        }
        ca_div_end();
        if (collapsed) skip_below = d;
    }
    free(links); free(flags);
}

void fl_scene_panel_build(Ca_Dock *dock, Ca_Window *window, void *data)
{
    Fl_ScenePanel *p = data; p->dock = dock; p->window = window;
    if (!p->keys) p->keys=ca_frame_effect(ca_window_instance(window),check_keys,p);
    const Fl_Scene *s = fl_project_scene(p->project, fl_scene_edit_scene(p->edit));
    size_t scenes = fl_project_scene_count(p->project), entities = s ? s->entity_count : 0;
    free(p->rows); p->rows = NULL; p->row_count = 0;
    if (!p->thumbnails) p->thumbnails = fl_scene_thumbnails_create(ca_window_instance(window));
    fl_scene_thumbnails_retain(p->thumbnails, p->project);
    if (scenes > SIZE_MAX - entities || scenes + entities > SIZE_MAX / sizeof(*p->rows)) return;
    p->rows = calloc(scenes + entities + 1, sizeof(*p->rows));
    if (!p->rows) return;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_SCENE});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_SCENE_TOOLBAR});
    ca_btn_begin(&(Ca_BtnDesc){.text = "+ Scene", .on_click = add_scene, .click_data = p, .style = FL_CLASS_ACTION}); ca_btn_end();
    build_aspect_selector(p, s);
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_SCENE_GUTTER});
    p->scene_strip = ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_SCENE_STRIP});
    for (size_t i = 0; i < scenes; ++i) {
        const Fl_Scene *scene = fl_project_scene_at(p->project, i);
        Fl_Row *row = &p->rows[p->row_count++]; *row = (Fl_Row){.panel=p,.scene=scene->id};
        char id[48]; snprintf(id, sizeof(id), "scene-%llu", (unsigned long long)scene->id);
        row->bounds = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = id,
            .style = FL_CLASS_SCENE_DRAG, .on_drag_end = scene_drag_end, .drag_data = row, .drag_preview = true});
        bool selected = scene->id == fl_scene_edit_scene(p->edit);
        ca_btn_begin(&(Ca_BtnDesc){.on_click = select_row, .click_data = row,
            .direction = CA_VERTICAL, .style = selected ? FL_CLASS_CARD_SELECTED : FL_CLASS_CARD});
        Ca_Image *thumbnail = fl_scene_thumbnails_get(p->thumbnails, scene);
        if (thumbnail) ca_image(&(Ca_ImageDesc){.image = thumbnail, .width = FL_STYLE_SCENE_THUMB_DISPLAY_W,
            .height = FL_STYLE_SCENE_THUMB_DISPLAY_H, .corner_radius = FL_STYLE_SCENE_THUMB_RADIUS, .style = FL_CLASS_SCENE_THUMB});
        ca_div_begin(&(Ca_DivDesc){.position = CA_POSITION_ABSOLUTE, .style = selected ? FL_CLASS_SCENE_GLASS_SELECTED : FL_CLASS_SCENE_GLASS, .no_hover = true});
        ca_div_end();
        char duration[32]; snprintf(duration, sizeof(duration), "%.2fs", scene->duration / 120000.0);
        ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .position = CA_POSITION_ABSOLUTE, .style = FL_CLASS_SCENE_LABEL, .no_hover = true});
        ca_text(&(Ca_TextDesc){.text = scene->name, .style = FL_CLASS_SCENE_NAME});
        ca_text(&(Ca_TextDesc){.text = duration, .style = FL_CLASS_SCENE_TIME});
        ca_div_end();
        ca_btn_end();
        const char *items[] = {"Rename", "Move earlier", "Move later", "Delete scene"};
        ca_context_menu(&(Ca_CtxMenuDesc){.items = items, .item_count = 4, .on_select = scene_menu, .select_data = row});
        ca_div_end();
    }
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.position = CA_POSITION_ABSOLUTE, .style = FL_CLASS_SCENE_RIM, .no_hover = true}); ca_div_end();
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_SCENE_BODY});
    ca_split_begin(&(Ca_SplitDesc){.direction = CA_HORIZONTAL, .ratio = .23f,
        .fixed_pane = CA_SPLIT_FIXED_FIRST, .fixed_size = FL_STYLE_HIERARCHY_W, .id = "scene-body"});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_HIERARCHY_PANE});
    p->hierarchy=ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_HIERARCHY});
    build_hierarchy(p, s);
    ca_div_end();
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_VIEW});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_VIEW_GUTTER});
    ca_div_begin(&(Ca_DivDesc){.direction=CA_VERTICAL,.id="scene-stage",.style=FL_CLASS_STAGE,
        .on_drag_start=stage_start,.on_drag=stage_move,.on_drag_end=stage_end,.drag_data=p,.drag_buttons=3,
        .on_scroll=stage_scroll,.scroll_data=p});
    p->viewport = ca_viewport(&(Ca_ViewportDesc){.on_render = fl_scene_preview_render, .render_data = p->preview, .id = "scene-viewport", .style = FL_CLASS_VIEWPORT});
    if (p->viewport) ca_viewport_request_redraw(p->viewport);
    ca_div_end();
    const char *canvas_items[]={"Center target frame",fl_scene_edit_snap(p->edit) ? "Disable grid snapping" : "Enable grid snapping"};
    ca_context_menu(&(Ca_CtxMenuDesc){.items=canvas_items,.item_count=2,.on_select=canvas_menu,.select_data=p});
    ca_div_begin(&(Ca_DivDesc){.position = CA_POSITION_ABSOLUTE, .style = FL_CLASS_VIEW_RIM, .no_hover = true}); ca_div_end();
    ca_div_end();
    ca_div_end(); ca_split_end(); ca_div_end(); ca_div_end();
}

/* Test a window-pixel point against a retained div's last laid-out bounds. */
static bool inside_div(Ca_Div *div,double x,double y)
{
    float bx,by,w,h;if (!div) return false;
    ca_div_screen_rect(div,&bx,&by,&w,&h);
    return x>=bx&&y>=by&&x<=bx+w&&y<=by+h;
}
bool fl_scene_panel_drop_target(Fl_ScenePanel *p,double screen_x,double screen_y,Fl_SceneDropTarget *out)
{
    if (!p||!out||!p->window||!p->viewport||!fl_project_scene(p->project,fl_scene_edit_scene(p->edit))) return false;
    int wx,wy,width,height;ca_window_get_rect(p->window,&wx,&wy,&width,&height);(void)height;
    float bx,by,w,h;ca_div_screen_rect(ca_window_content_div(p->window),&bx,&by,&w,&h);(void)bx;(void)by;(void)h;
    if (width<=0) return false;
    double scale=w/width,x=(screen_x-wx)*scale,y=(screen_y-wy)*scale,sx,sy;
    *out=(Fl_SceneDropTarget){.scene=fl_scene_edit_scene(p->edit)};
    if (inside_div(p->hierarchy,x,y)) {
        for (size_t i=0;i<p->row_count;++i) if (p->rows[i].entity&&inside_div(p->rows[i].bounds,x,y)) {out->parent=p->rows[i].entity;break;}
        return true;
    }
    if (!fl_scene_preview_point(p->preview,p->viewport,x,y,false,&sx,&sy)) return false;
    out->x=sx;out->y=sy;out->at_point=true;
    return true;
}
bool fl_scene_panel_drop_widget(Fl_ScenePanel *p,Fl_WidgetKind kind,double screen_x,double screen_y)
{
    Fl_SceneDropTarget target;
    return fl_scene_panel_drop_target(p,screen_x,screen_y,&target)&&fl_scene_edit_insert(p->edit,kind,target.parent,target.x,target.y);
}

#include "fl_scene_panel.h"
#include "style.h"
#include "core/fl_log.h"
#include "fl_scene_preview.h"
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
    bool gesturing;
    double drag_offset_x,drag_offset_y,drag_scale_x,drag_scale_y;
    Fl_Row *rows;
    size_t row_count;
    Fl_ScenePreview *preview;
    Fl_Fonts *fonts;
    Ca_Window *rename_window;
    Ca_TextInput *rename_input;
    Fl_Id rename_scene, rename_entity;
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

/* Save the project explicitly; automatic saves use the same engine boundary. */
static void save_project(Ca_Button *button, void *data)
{
    (void)button; Fl_ScenePanel *p = data;
    if (!fl_project_save(p->project)) FL_LOG_ERROR("project", "save failed; changes remain in memory");
}

/* Add a group or rectangle under parent, then select its stable identity. */
static void add_entity(Fl_ScenePanel *p, Fl_Id scene, Fl_Id parent, bool rectangle)
{
    Fl_Id id;
    if (fl_widget_create(p->project,scene,parent,rectangle ? FL_WIDGET_RECTANGLE : FL_WIDGET_GROUP,0,0,&id))
        fl_scene_edit_select(p->edit,scene,id,false);
}

/* Insert an entity at the active scene root. */
static void add_group(Ca_Button *button, void *data)
{
    (void)button; Fl_ScenePanel *p = data; add_entity(p, fl_scene_edit_scene(p->edit), 0, false);
}
static void add_rectangle(Ca_Button *button, void *data)
{
    (void)button; Fl_ScenePanel *p = data; add_entity(p, fl_scene_edit_scene(p->edit), 0, true);
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
    if (p->gesturing) fl_scene_edit_update(p->edit,event->x*p->drag_scale_x+p->drag_offset_x,
        event->y*p->drag_scale_y+p->drag_offset_y);
}
/* Finish pointer editing; project persistence observes the resulting operations. */
static void stage_end(const Ca_DragEvent *event,void *data)
{
    Fl_ScenePanel *p=data;
    if (!p->gesturing) return;
    stage_move(event,p);fl_scene_edit_end(p->edit,false);p->gesturing=false;
}
/* Escape rolls back only the active viewport gesture. */
static void check_keys(void *data)
{
    Fl_ScenePanel *p=data;
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
    if (!fl_event_subscribe(bus,&(Fl_SubscribeDesc){.type=selection,.fn=selection_changed,.user_data=p,.owner=p}).generation ||
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
    free(p->rows); free(p);
}

void fl_scene_panel_build(Ca_Dock *dock, Ca_Window *window, void *data)
{
    Fl_ScenePanel *p = data; p->dock = dock; p->window = window;
    if (!p->keys) p->keys=ca_frame_effect(ca_window_instance(window),check_keys,p);
    const Fl_Scene *s = fl_project_scene(p->project, fl_scene_edit_scene(p->edit));
    size_t scenes = fl_project_scene_count(p->project), entities = s ? s->entity_count : 0;
    free(p->rows); p->rows = NULL; p->row_count = 0;
    if (scenes > SIZE_MAX - entities || scenes + entities > SIZE_MAX / sizeof(*p->rows)) return;
    p->rows = calloc(scenes + entities + 1, sizeof(*p->rows));
    if (!p->rows) return;
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_SCENE});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TOOLBAR});
    ca_text(&(Ca_TextDesc){.text = "Scenes", .style = FL_CLASS_HEADING});
    ca_btn_begin(&(Ca_BtnDesc){.text = "+ Scene", .on_click = add_scene, .click_data = p, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_btn_begin(&(Ca_BtnDesc){.text = "Save project", .on_click = save_project, .click_data = p, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_div_end();
    p->scene_strip = ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_SCENE_STRIP});
    for (size_t i = 0; i < scenes; ++i) {
        const Fl_Scene *scene = fl_project_scene_at(p->project, i);
        Fl_Row *row = &p->rows[p->row_count++]; *row = (Fl_Row){.panel=p,.scene=scene->id};
        char id[48]; snprintf(id, sizeof(id), "scene-%llu", (unsigned long long)scene->id);
        row->bounds = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = id,
            .style = FL_CLASS_SCENE_DRAG, .on_drag_end = scene_drag_end, .drag_data = row, .drag_preview = true});
        ca_btn_begin(&(Ca_BtnDesc){.on_click = select_row, .click_data = row,
            .direction = CA_VERTICAL, .style = scene->id == fl_scene_edit_scene(p->edit) ? FL_CLASS_CARD_SELECTED : FL_CLASS_CARD});
        char info[96]; snprintf(info, sizeof(info), "%zu   %.2fs", i + 1, scene->duration / 120000.0);
        ca_text(&(Ca_TextDesc){.text = info, .style = FL_CLASS_MUTED});
        ca_text(&(Ca_TextDesc){.text = scene->name, .style = FL_CLASS_HEADING});
        ca_btn_end(); ca_div_end();
        const char *items[] = {"Rename", "Move earlier", "Move later", "Delete scene"};
        ca_context_menu(&(Ca_CtxMenuDesc){.items = items, .item_count = 4, .on_select = scene_menu, .select_data = row});
    }
    ca_div_end();
    ca_split_begin(&(Ca_SplitDesc){.direction = CA_HORIZONTAL, .ratio = .23f,
        .fixed_pane = CA_SPLIT_FIXED_FIRST, .fixed_size = FL_STYLE_HIERARCHY_W, .id = "scene-body"});
    p->hierarchy=ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_HIERARCHY});
    ca_text(&(Ca_TextDesc){.text = "Hierarchy", .style = FL_CLASS_HEADING});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_TOOLBAR});
    ca_btn_begin(&(Ca_BtnDesc){.text = "+ Group", .on_click = add_group, .click_data = p, .disabled = !s, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_btn_begin(&(Ca_BtnDesc){.text = "+ Rectangle", .on_click = add_rectangle, .click_data = p, .disabled = !s, .style = FL_CLASS_ACTION}); ca_btn_end();
    ca_div_end();
    /* Build child/sibling adjacency once, then walk it without recursion. */
    size_t *links = entities < SIZE_MAX / sizeof(size_t) / 3 ? malloc((entities * 3 + 1) * sizeof(size_t)) : NULL;
    size_t first_root = SIZE_MAX;
    if (links) {
        size_t *first = links, *next = links + entities, *parents = links + entities * 2;
        for (size_t i = 0; i < entities; ++i) first[i] = next[i] = SIZE_MAX;
        for (size_t i = entities; i > 0; ) {
            --i; const Fl_Entity *parent = fl_scene_entity(s, s->entities[i].parent);
            size_t *head = parent ? &first[parent - s->entities] : &first_root;
            next[i] = *head; *head = i;
        }
        size_t i = first_root, depth = 0;
        while (i != SIZE_MAX) {
            const Fl_Entity *e = &s->entities[i];
            Fl_Row *row = &p->rows[p->row_count++]; *row = (Fl_Row){.panel=p,.scene=s->id,.entity=e->id};
            char id[48]; snprintf(id, sizeof(id), "entity-%llu", (unsigned long long)e->id);
            row->bounds=ca_div_begin(&(Ca_DivDesc){.direction=CA_VERTICAL,.style=FL_CLASS_ENTITY_ROW,.id=id});
            ca_btn_begin(&(Ca_BtnDesc){.on_click = select_row, .click_data = row, .id = id,
                .style = fl_scene_edit_contains(p->edit,e->id) ? FL_CLASS_ENTITY_SELECTED : FL_CLASS_ENTITY});
            if (depth) ca_spacer(&(Ca_SpacerDesc){.width = (float)depth * FL_STYLE_TREE_INDENT});
            ca_text(&(Ca_TextDesc){.text = e->name, .style = e->visible ? FL_CLASS_LABEL : FL_CLASS_MUTED});
            ca_btn_end();
            const char *items[] = {"Rename", "Add child group", "Add child rectangle", "Toggle visibility", "Move to root", "Move up", "Move down", "Delete subtree", "Parent to selected"};
            ca_context_menu(&(Ca_CtxMenuDesc){.items = items, .item_count = 9, .on_select = entity_menu, .select_data = row});
            ca_div_end();
            if (first[i] != SIZE_MAX) { parents[depth++] = i; i = first[i]; }
            else {
                while (next[i] == SIZE_MAX && depth) i = parents[--depth];
                i = next[i];
            }
        }
        free(links);
    }
    if (!entities) ca_text(&(Ca_TextDesc){.text = s ? "Add an entity to begin." : "Create a scene to begin.", .style = FL_CLASS_MUTED});
    ca_div_end();
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_VIEW});
    char title[160]; snprintf(title, sizeof(title), "Viewport   %s", s ? s->name : "No scene");
    ca_text(&(Ca_TextDesc){.text = title, .style = FL_CLASS_VIEW_TITLE});
    ca_div_begin(&(Ca_DivDesc){.direction=CA_VERTICAL,.id="scene-stage",.style=FL_CLASS_STAGE,
        .on_drag_start=stage_start,.on_drag=stage_move,.on_drag_end=stage_end,.drag_data=p});
    p->viewport = ca_viewport(&(Ca_ViewportDesc){.on_render = fl_scene_preview_render, .render_data = p->preview, .id = "scene-viewport", .style = FL_CLASS_VIEWPORT});
    if (p->viewport) ca_viewport_request_redraw(p->viewport);
    ca_div_end();
    ca_div_end(); ca_split_end(); ca_div_end();
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

#include "fl_scene_panel.h"
#include "style.h"
#include "core/fl_log.h"
#include "fl_scene_preview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Fl_Row { struct Fl_ScenePanel *panel; Fl_Id scene, entity; } Fl_Row;
struct Fl_ScenePanel {
    Fl_Project *project;
    Fl_EventBus *bus;
    Ca_Dock *dock;
    Ca_Window *window;
    Fl_Id scene, selected;
    Fl_Row *rows;
    size_t row_count;
    Fl_ScenePreview *preview;
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
    if (!fl_project_scene(p->project, p->scene)) {
        const Fl_Scene *first = fl_project_scene_at(p->project, 0); p->scene = first ? first->id : 0;
    }
    if (!fl_scene_entity(fl_project_scene(p->project, p->scene), p->selected)) p->selected = 0;
    if (p->dock) ca_dock_invalidate_panel(p->dock, "scene");
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
    row.panel->scene = row.scene; row.panel->selected = row.entity;
    ca_dock_invalidate_panel(row.panel->dock, "scene");
}

/* Create a scene at the end of the project's render order. */
static void add_scene(Ca_Button *button, void *data)
{
    (void)button; Fl_ScenePanel *p = data; Fl_Id id;
    char name[64]; snprintf(name, sizeof(name), "Scene %zu", fl_project_scene_count(p->project) + 1);
    if (apply(p, (Fl_Op){.kind = FL_OP_SCENE_ADD, .name = name}, &id)) { p->scene = id; p->selected = 0; }
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
    if (!apply(p, (Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = scene, .parent = parent,
                         .name = rectangle ? "Rectangle" : "Group"}, &id)) return;
    p->scene = scene; p->selected = id;
    if (rectangle) {
        const unsigned char rgba[4] = FL_STYLE_SCENE_RECT_RGBA;
        apply(p, (Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene, .entity = id,
                        .component = {FL_COMPONENT_RECTANGLE, 1, sizeof(rgba), rgba}}, NULL);
    }
}

/* Insert an entity at the active scene root. */
static void add_group(Ca_Button *button, void *data)
{
    (void)button; Fl_ScenePanel *p = data; add_entity(p, p->scene, 0, false);
}
static void add_rectangle(Ca_Button *button, void *data)
{
    (void)button; Fl_ScenePanel *p = data; add_entity(p, p->scene, 0, true);
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
    else if (index == 8 && p->selected && p->selected != e->id)
        apply(p, (Fl_Op){.kind = FL_OP_ENTITY_PARENT, .scene = row.scene, .entity = row.entity, .parent = p->selected}, NULL);
    else if (index == 5 || index == 6) {
        size_t at = (size_t)(e - s->entities), to = at;
        if (index == 5) { while (to) { --to; if (s->entities[to].parent == e->parent) break; } }
        else { while (++to < s->entity_count) if (s->entities[to].parent == e->parent) break; }
        if (to < s->entity_count && s->entities[to].parent == e->parent)
            apply(p, (Fl_Op){.kind = FL_OP_ENTITY_MOVE, .scene = row.scene, .entity = row.entity, .position = to}, NULL);
    }
}

Fl_ScenePanel *fl_scene_panel_create(Fl_Project *project, Fl_EventBus *bus)
{
    if (!project || !bus) return NULL;
    Fl_ScenePanel *p = calloc(1, sizeof(*p)); if (!p) return NULL;
    p->project = project; p->bus = bus;
    p->preview = fl_scene_preview_create(project);
    if (!p->preview) { free(p); return NULL; }
    const Fl_Scene *first = fl_project_scene_at(project, 0); p->scene = first ? first->id : 0;
    Fl_EventType type = fl_event_type_register(bus, FL_EVENT_PROJECT_CHANGED, sizeof(Fl_ProjectChange));
    if (type == FL_EVENT_TYPE_INVALID || !fl_event_subscribe(bus, &(Fl_SubscribeDesc){.type = type, .fn = changed, .user_data = p, .owner = p}).generation) {
        fl_scene_preview_destroy(p->preview); free(p); return NULL;
    }
    return p;
}

void fl_scene_panel_teardown(void *data)
{
    Fl_ScenePanel *p = data; p->dock = NULL; p->window = NULL;
}

void fl_scene_panel_destroy(Fl_ScenePanel *p)
{
    if (!p) return;
    if (p->rename_window) ca_window_destroy(p->rename_window);
    fl_scene_panel_teardown(p); fl_event_unsubscribe_owner(p->bus, p);
    fl_scene_preview_destroy(p->preview);
    free(p->rows); free(p);
}

void fl_scene_panel_build(Ca_Dock *dock, Ca_Window *window, void *data)
{
    Fl_ScenePanel *p = data; p->dock = dock; p->window = window;
    const Fl_Scene *s = fl_project_scene(p->project, p->scene);
    if (!fl_scene_preview_set_scene(p->preview, p->scene)) FL_LOG_ERROR("scene", "preview allocation failed");
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
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_SCENE_STRIP});
    for (size_t i = 0; i < scenes; ++i) {
        const Fl_Scene *scene = fl_project_scene_at(p->project, i);
        Fl_Row *row = &p->rows[p->row_count++]; *row = (Fl_Row){p, scene->id, 0};
        char id[48]; snprintf(id, sizeof(id), "scene-%llu", (unsigned long long)scene->id);
        ca_btn_begin(&(Ca_BtnDesc){.on_click = select_row, .click_data = row, .id = id,
            .direction = CA_VERTICAL, .style = scene->id == p->scene ? FL_CLASS_CARD_SELECTED : FL_CLASS_CARD});
        char info[96]; snprintf(info, sizeof(info), "%zu   %.2fs", i + 1, scene->duration / 120000.0);
        ca_text(&(Ca_TextDesc){.text = info, .style = FL_CLASS_MUTED});
        ca_text(&(Ca_TextDesc){.text = scene->name, .style = FL_CLASS_HEADING});
        ca_btn_end();
        const char *items[] = {"Rename", "Move earlier", "Move later", "Delete scene"};
        ca_context_menu(&(Ca_CtxMenuDesc){.items = items, .item_count = 4, .on_select = scene_menu, .select_data = row});
    }
    ca_div_end();
    ca_split_begin(&(Ca_SplitDesc){.direction = CA_HORIZONTAL, .ratio = .23f,
        .fixed_pane = CA_SPLIT_FIXED_FIRST, .fixed_size = FL_STYLE_HIERARCHY_W, .id = "scene-body"});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_HIERARCHY});
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
            Fl_Row *row = &p->rows[p->row_count++]; *row = (Fl_Row){p, s->id, e->id};
            char id[48]; snprintf(id, sizeof(id), "entity-%llu", (unsigned long long)e->id);
            ca_btn_begin(&(Ca_BtnDesc){.on_click = select_row, .click_data = row, .id = id,
                .style = e->id == p->selected ? FL_CLASS_ENTITY_SELECTED : FL_CLASS_ENTITY});
            if (depth) ca_spacer(&(Ca_SpacerDesc){.width = (float)depth * FL_STYLE_TREE_INDENT});
            ca_text(&(Ca_TextDesc){.text = e->name, .style = e->visible ? FL_CLASS_LABEL : FL_CLASS_MUTED});
            ca_btn_end();
            const char *items[] = {"Rename", "Add child group", "Add child rectangle", "Toggle visibility", "Move to root", "Move up", "Move down", "Delete subtree", "Parent to selected"};
            ca_context_menu(&(Ca_CtxMenuDesc){.items = items, .item_count = 9, .on_select = entity_menu, .select_data = row});
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
    Ca_Viewport *viewport = ca_viewport(&(Ca_ViewportDesc){.on_render = fl_scene_preview_render, .render_data = p->preview, .id = "scene-viewport", .style = FL_CLASS_VIEWPORT});
    if (viewport) ca_viewport_request_redraw(viewport);
    ca_div_end(); ca_split_end(); ca_div_end();
}

#include "flow_test.h"
#include "components/fl_animation.h"
#include "components/fl_component.h"
#include "components/fl_script.h"
#include "time/fl_scene_state.h"
#include "doc/fl_scene_geometry.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SECOND FL_TICKS_PER_SECOND

/* Store source as the entity's own repository script (created on first use) and attach it, through the validated boundary. */
static bool script(Fl_Project *project, Fl_Id scene, Fl_Id entity, const char *source, bool enabled)
{
    char name[48]; snprintf(name, sizeof(name), "entity %llu", (unsigned long long)entity);
    const Fl_ScriptSource *existing = fl_scene_script_named(fl_project_scene(project, scene), name);
    Fl_Id id = existing ? existing->id : 0, created = 0;
    if (!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene, .script = id, .name = existing ? NULL : name,
                          .source = source, .source_size = strlen(source)}, &created)) return false;
    if (!id) id = created;
    Fl_Component component = {0};
    bool ok = fl_script_encode(&(Fl_Script){id, enabled}, &component);
    if (ok) ok = fl_project_apply(project, &(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=component}, NULL);
    fl_component_clear(&component);
    return ok;
}

/* Sample document time directly; evaluation never changes base components or revisions. */
static double sample(Fl_SceneState *state, const Fl_Scene *scene, Fl_Id entity, Fl_Tick tick)
{
    CHECK(fl_scene_state_update(state, scene, tick));
    const Fl_Entity *e = fl_scene_entity(scene, entity);
    CHECK(e != NULL);
    return e ? state->entities[e - scene->entities].transform.x : 0;
}

/* Runtime errors, budgets and invalid outputs retain all evaluated base values. */
static void failure(Fl_Project *project, const Fl_Scene *scene, Fl_Id entity, Fl_SceneState *state, const char *source, const char *message)
{
    CHECK(script(project, scene->id, entity, source, true));
    CHECK(sample(state, scene, entity, SECOND) == 25);
    const char *error = fl_script_runtime_error(state->scripts, (size_t)(fl_scene_entity(scene, entity) - scene->entities));
    CHECK(strstr(error, message) != NULL);
}

/* The repository: shared scripts, unique names, atomic validation, cascading removal and persistence. Closes project. */
static void repository(Fl_Project *project, Fl_Id scene_id, const char *directory)
{
    const Fl_Scene *scene = fl_project_scene(project, scene_id);
    Fl_Id one = 0, two = 0, counter = 0, other = 0;
    CHECK(fl_widget_create(project, scene_id, 0, FL_WIDGET_RECTANGLE, 0, 0, &one) && fl_widget_create(project, scene_id, 0, FL_WIDGET_RECTANGLE, 0, 0, &two));
    const char *source = "function on_evaluate(self, t) self.transform.x = self.transform.x + 10 * t end";
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .name = "Counter"}, &counter));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .source = source, .source_size = strlen(source)}, &counter));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .name = "", .source = source, .source_size = strlen(source)}, &counter));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .name = "Bad", .source = "a\0b", .source_size = 3}, &counter));
    const size_t base = scene->script_count;
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .name = "Counter", .source = source, .source_size = strlen(source)}, &counter) && counter);
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .name = "Counter", .source = source, .source_size = strlen(source)}, &other));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .name = "Other", .source = "x = 1", .source_size = 5}, &other));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .script = other, .name = "Counter"}, NULL));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .script = other}, NULL));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .script = 999999, .name = "Ghost"}, NULL));
    CHECK(scene->script_count == base + 2 && fl_scene_script_named(scene, "Counter")->id == counter);
    Fl_Component attach = {0};
    CHECK(fl_script_encode(&(Fl_Script){999999, true}, &attach));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene_id, .entity = one, .component = attach}, NULL));
    fl_component_clear(&attach);
    CHECK(fl_script_encode(&(Fl_Script){counter, true}, &attach));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene_id, .entity = one, .component = attach}, NULL));
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = scene_id, .entity = two, .component = attach}, NULL));
    fl_component_clear(&attach);
    CHECK(fl_scene_script_uses(scene, counter) == 2 && fl_scene_script_uses(scene, other) == 0);
    Fl_SceneState state = {0};
    CHECK(fl_scene_state_update(&state, scene, SECOND));
    const size_t first = (size_t)(fl_scene_entity(scene, one) - scene->entities), second_index = (size_t)(fl_scene_entity(scene, two) - scene->entities);
    CHECK(state.entities[first].transform.x == 10 && state.entities[second_index].transform.x == 10);
    const char *faster = "function on_evaluate(self, t) self.transform.x = 100 * t end";
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .script = counter, .source = faster, .source_size = strlen(faster)}, NULL));
    CHECK(fl_scene_state_update(&state, scene, SECOND));
    CHECK(state.entities[first].transform.x == 100 && state.entities[second_index].transform.x == 100);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_SET, .scene = scene_id, .script = counter, .name = "Ramp"}, NULL));
    CHECK(fl_scene_script_named(scene, "Ramp") && !fl_scene_script_named(scene, "Counter") && fl_scene_script_uses(scene, counter) == 2);
    CHECK(fl_project_save(project));
    fl_project_destroy(project);
    project = fl_project_open(directory, NULL);
    CHECK(project);
    if (!project) return;
    scene = fl_project_scene(project, scene_id);
    const Fl_ScriptSource *ramp = fl_scene_script_named(scene, "Ramp");
    CHECK(ramp && ramp->id == counter && !strcmp(ramp->source, faster) && ramp->source_size == strlen(faster) && fl_scene_script_uses(scene, counter) == 2);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_REMOVE, .scene = scene_id, .script = counter}, NULL));
    CHECK(!fl_scene_script(scene, counter) && fl_scene_script_uses(scene, counter) == 0 && !fl_entity_component(fl_scene_entity(scene, one), FL_COMPONENT_SCRIPT));
    CHECK(!fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_REMOVE, .scene = scene_id, .script = counter}, NULL));
    Fl_Snapshot *snapshot = fl_project_snapshot(project);
    CHECK(snapshot && fl_project_apply(project, &(Fl_Op){.kind = FL_OP_SCRIPT_REMOVE, .scene = scene_id, .script = other}, NULL));
    CHECK(fl_project_restore(project, snapshot));
    scene = fl_project_scene(project, scene_id);
    CHECK(fl_scene_script(scene, other) != NULL);
    fl_snapshot_destroy(snapshot);
    fl_scene_state_clear(&state);
    fl_project_destroy(project);
}

/* Exercise real Lua execution, component ownership, conflict rejection and persistence. */
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char path[FL_PATH_MAX];
    CHECK(fl_fs_mkdir_p(argv[1]));
    CHECK(fl_fs_join(path, sizeof(path), argv[1], "project.flow")); fl_fs_remove(path);
    Fl_Project *project=fl_project_create(argv[1], "Lua components", NULL);
    CHECK(project); if (!project) return 1;
    Fl_Id scene_id=fl_project_scene_at(project,0)->id, box, second;
    CHECK(fl_widget_create(project,scene_id,0,FL_WIDGET_RECTANGLE,25,40,&box));
    CHECK(fl_widget_create(project,scene_id,box,FL_WIDGET_RECTANGLE,10,20,&second));
    const Fl_Scene *scene=fl_project_scene(project,scene_id);
    const Fl_Entity *entity=fl_scene_entity(scene,box);
    CHECK(entity->components[0].type==FL_COMPONENT_TRANSFORM);
    uint64_t revision=scene->revision;
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=box,.component={.type=FL_COMPONENT_TRANSFORM}},NULL));
    CHECK(scene->revision==revision);
    Fl_SceneState state={0};
    const char *motion="function on_evaluate(self, time) self.transform.x = self.transform.x + time * 120 end";
    CHECK(script(project,scene_id,box,motion,true));
    revision=scene->revision;
    CHECK(sample(&state,scene,box,SECOND)==145);
    CHECK(sample(&state,scene,box,2*SECOND)==265);
    CHECK(sample(&state,scene,box,SECOND/2)==85);
    CHECK(sample(&state,scene,box,SECOND)==145);
    CHECK(scene->revision==revision);
    Fl_Transform transform;
    CHECK(fl_transform_decode(fl_entity_component(fl_scene_entity(scene,box),FL_COMPONENT_TRANSFORM),&transform));
    CHECK(transform.x==25);
    CHECK(!*fl_script_runtime_error(state.scripts,0));
    Fl_SceneGeometry geometry={0};
    CHECK(fl_scene_geometry_update(&geometry,scene,&state));
    CHECK(fabs(geometry.entities[1].world.v[2] - 155)<1e-9);
    fl_scene_geometry_clear(&geometry);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_KEY_SET,.scene=scene_id,.entity=box,.property=FL_PROP_Y,
        .anchor=fl_anchor_at(scene,0),.value={100}},NULL));
    const Fl_Animation *animation=fl_entity_animation(fl_scene_entity(scene,box));
    CHECK(animation && animation->track_count==1 && animation->tracks[0].property==FL_PROP_Y);
    CHECK(script(project,scene_id,box,motion,true));
    CHECK(sample(&state,scene,box,SECOND)==145 && state.entities[0].transform.y==100);
    CHECK(script(project,scene_id,second,"function on_evaluate(self, time) self.transform.x = time * 10 end",true));
    CHECK(sample(&state,scene,second,SECOND)==10);
    CHECK(sample(&state,scene,box,SECOND)==145);
    /* Stateful per-frame hooks are not part of the evaluation contract. */
    failure(project,scene,box,&state,"function on_update(self,dt,time) self.transform.x=99 end","on_evaluate");
    failure(project,scene,box,&state,"function on_evaluate(self,t) self.animation:play('Run') end","nil");
    CHECK(sample(&state,scene,box,2*SECOND)==25);
    CHECK(script(project,scene_id,box,motion,true));
    CHECK(sample(&state,scene,box,SECOND)==145);
    /* A keyed Inspector-like configure must not bake evaluated script channels into base values. */
    CHECK(fl_transform_decode(fl_entity_component(fl_scene_entity(scene,box),FL_COMPONENT_TRANSFORM),&transform));
    transform.y=state.entities[0].transform.y;
    unsigned char bytes[56]; fl_transform_encode(&transform,bytes);
    Fl_Component component={FL_COMPONENT_TRANSFORM,1,56,bytes};
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_CONFIGURE,.scene=scene_id,.entity=box,.name="Renamed",.visible=true,
        .components=&component,.component_count=1,.keyed=true,.tick=SECOND},NULL));
    CHECK(fl_transform_decode(fl_entity_component(fl_scene_entity(scene,box),FL_COMPONENT_TRANSFORM),&transform) && transform.x==25);
    CHECK(fl_entity_animation(fl_scene_entity(scene,box))->tracks[0].key_count==1);
    /* Growing a copied animation during configure must remain owned by the replacement component. */
    for (unsigned k=1;k<7;++k) {
        transform.y=100+k;fl_transform_encode(&transform,bytes);
        CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_CONFIGURE,.scene=scene_id,.entity=box,.name="Renamed",.visible=true,
            .components=&component,.component_count=1,.keyed=true,.tick=(Fl_Tick)k*SECOND/2},NULL));
    }
    CHECK(fl_entity_animation(fl_scene_entity(scene,box))->tracks[0].key_count==7);
    CHECK(fl_project_save(project));
    fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(argv[1],NULL);CHECK(project);if (!project) return 1;
    scene=fl_project_scene(project,scene_id);
    CHECK(!fl_project_dirty(project));
    CHECK(sample(&state,scene,box,SECOND)==145);
    CHECK(fl_entity_animation(fl_scene_entity(scene,box))->tracks[0].key_count==7);
    /* The global environment and random stream reset for each evaluation. */
    CHECK(script(project,scene_id,box,"n=(n or 0)+1; function on_evaluate(self,t) self.transform.x=n+math.random() end",true));
    double a=sample(&state,scene,box,SECOND);
    CHECK(sample(&state,scene,box,2*SECOND)==a && a>1 && a<2);
    failure(project,scene,box,&state,"function on_evaluate(self,t) while true do end end","instruction limit");
    failure(project,scene,box,&state,"function on_evaluate(self,t) self.transform.x=0/0 end","invalid x");
    failure(project,scene,box,&state,"function on_evaluate(self,t) self.transform.x=99; error('failed') end","failed");
    failure(project,scene,box,&state,"function on_evaluate(self,t) self.transform.x=string.rep('a',1000000000) end","memory");
    failure(project,scene,box,&state,"function on_evaluate(","expected");
    failure(project,scene,box,&state,"function on_evaluate(self,t) io.open('file') end","nil");
    failure(project,scene,box,&state,"function on_evaluate(self,t) pcall(function() end) end","nil");
    failure(project,scene,box,&state,"function on_evaluate(self,t) os.execute('echo nope') end","nil");
    failure(project,scene,box,&state,"function on_evaluate(self,t) require('x') end","nil");
    failure(project,scene,box,&state,"function on_evaluate(self,t) self.transform=nil end","table");
    failure(project,scene,box,&state,"function on_evaluate(self,t) math.randomseed() end","number expected");
    failure(project,scene,box,&state,"function on_evaluate(self,t) self.transform.x=tonumber(tostring({})) end","scalar");
    CHECK(script(project,scene_id,box,"function on_evaluate(self,t) local sum=0; for k,v in pairs({c=3,a=1,b=2}) do sum=sum*10+v end; self.transform.x=sum end",true));
    CHECK(sample(&state,scene,box,SECOND)==123);
    CHECK(sample(&state,scene,box,2*SECOND)==123);
    /* Scripts may replace a text entity's content; the document keeps its own text. */
    Fl_Id label = 0;
    CHECK(fl_widget_create(project, scene_id, 0, FL_WIDGET_TEXT, 0, 0, &label));
    CHECK(script(project, scene_id, label, "function on_evaluate(self, t) self.text = self.text .. ' ' .. math.floor(t) end", true));
    scene = fl_project_scene(project, scene_id);
    const size_t label_index = (size_t)(fl_scene_entity(scene, label) - scene->entities);
    sample(&state, scene, label, 3 * SECOND);
    CHECK(state.entities[label_index].text_override && !strcmp(state.entities[label_index].text, "Text 3"));
    const Fl_Component *base_text = fl_entity_component(fl_scene_entity(scene, label), FL_COMPONENT_TEXT);
    CHECK(base_text->size == 4 && !memcmp(base_text->data, "Text", 4));
    CHECK(script(project, scene_id, label, "function on_evaluate(self,t) self.text = 5 end", true));
    sample(&state, scene, label, SECOND);
    CHECK(strstr(fl_script_runtime_error(state.scripts, label_index), "string") && !state.entities[label_index].text_override);
    CHECK(script(project, scene_id, label, "function on_evaluate(self, t) self.text = self.text end", true));
    sample(&state, scene, label, SECOND);
    CHECK(!state.entities[label_index].text_override);
    CHECK(fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ENTITY_REMOVE, .scene = scene_id, .entity = label}, NULL));
    /* A corrected source recovers immediately after compilation/runtime/memory errors. */
    CHECK(script(project,scene_id,box,motion,true));
    CHECK(sample(&state,scene,box,SECOND)==145);
    CHECK(script(project,scene_id,box,motion,false));
    CHECK(sample(&state,scene,box,SECOND)==25);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=box,.component={.type=FL_COMPONENT_ANIMATION}},NULL));
    CHECK(!fl_entity_animation(fl_scene_entity(scene,box)) && !fl_scene_track_count(scene));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene_id,.entity=box,.component={.type=FL_COMPONENT_SCRIPT}},NULL));
    CHECK(sample(&state,scene,box,SECOND)==25);
    Fl_Component invalid={0};
    CHECK(!fl_script_encode(&(Fl_Script){.script=0,.enabled=true},&invalid));
    CHECK(!fl_script_source_valid("",0) && !fl_script_source_valid("a\0b",3) && fl_script_source_valid("x",1));
    CHECK(!fl_script_name_valid("") && !fl_script_name_valid("a\nb") && fl_script_name_valid("Counter"));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_REMOVE,.scene=scene_id,.entity=box},NULL));
    CHECK(scene->entity_count==0 && fl_scene_track_count(scene)==0);
    CHECK(fl_project_save(project));
    fl_scene_state_clear(&state);
    repository(project, scene_id, argv[1]);
    return FLOW_TEST_RESULT("fl_script_tests");
}

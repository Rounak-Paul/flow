#include "flow_test.h"
#include "components/fl_events.h"
#include "components/fl_script.h"
#include "time/fl_scene_state.h"
#include <string.h>

/* Event delivery is ordered, retime-aware and identical on forward/backward deterministic replay. */
int main(int argc, char **argv)
{
    CHECK(argc==2); if (argc!=2) return FLOW_TEST_RESULT("fl_events_tests");
    Fl_Project *project=fl_project_create(argv[1],"Events",NULL); CHECK(project); if (!project) return FLOW_TEST_RESULT("fl_events_tests");
    Fl_Id scene=fl_project_scene_at(project,0)->id,entity=0,asset=0;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="Counter"},&entity));
    const char *source="return {outputs={self={x='replace',blur='replace',shadow_color='replace'}},initialize=function(ctx) ctx.state.n=0; for _,e in ipairs(ctx.events) do ctx.state.n=ctx.state.n+e.value end end,step=function(ctx) for _,e in ipairs(ctx.events) do ctx.state.n=ctx.state.n+e.value end end,sample=function(ctx) assert(#ctx.events==0);ctx.entity.pose.x=ctx.state.n;ctx.entity.style.blur=ctx.time.seconds;ctx.entity.style.shadow_color={10,20,30,255} end}";
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=FL_ASSET_LUA,.name="Counter",.source=source,.source_size=strlen(source)},&asset));
    Fl_Component script={0}; CHECK(fl_script_encode(&(Fl_Script){.script=asset,.enabled=true},&script));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=script},NULL)); fl_component_clear(&script);
    Fl_Id beat=fl_project_scene(project,scene)->beats[0].id; char input[1024];
    snprintf(input,sizeof(input),"{\"next_id\":\"4\",\"points\":[{\"id\":\"3\",\"at\":{\"kind\":\"absolute\",\"beat\":\"0\",\"end\":false,\"offset\":\"120000\",\"position\":0},\"name\":\"later\",\"value\":100},{\"id\":\"1\",\"at\":{\"kind\":\"absolute\",\"beat\":\"0\",\"end\":false,\"offset\":\"0\",\"position\":0},\"name\":\"start\",\"value\":1},{\"id\":\"2\",\"at\":{\"kind\":\"beat\",\"beat\":\"%llu\",\"end\":false,\"offset\":\"60000\",\"position\":0},\"name\":\"middle\",\"value\":10}]}",(unsigned long long)beat);
    Fl_Json c={0}; cJSON *json=fl_json_parse(&c,input,strlen(input)); CHECK(json); Fl_Component events={0};
    CHECK(fl_events_encode(json,&events)); CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=entity,.component=events},NULL));
    cJSON_Delete(json); fl_component_clear(&events);
    Fl_SceneState state={0};
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000)); CHECK(state.entities[0].pose.x==111);
    CHECK(state.entities[0].style.blur==1 && state.entities[0].style.shadow_color[2]==30);
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),0)); CHECK(state.entities[0].pose.x==1);
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),60000)); CHECK(state.entities[0].pose.x==11);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind=FL_OP_BEAT_SPLIT,.scene=scene,.beat=beat,.tick=30000,.name="Right"},NULL));
    CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),60000)); CHECK(state.entities[0].pose.x==11);
    CHECK(fl_project_save(project)); fl_scene_state_clear(&state); fl_project_destroy(project);
    project=fl_project_open(argv[1],NULL); CHECK(project);
    if (project) {CHECK(fl_scene_state_update(&state,fl_project_scene(project,scene),120000)); CHECK(state.entities[0].pose.x==111); fl_scene_state_clear(&state); fl_project_destroy(project);}
    return FLOW_TEST_RESULT("fl_events_tests");
}

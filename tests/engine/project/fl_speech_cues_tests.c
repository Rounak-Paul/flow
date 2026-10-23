#include "flow_test.h"
#include "project/fl_speech_cues.h"
#include "project/fl_manuscript.h"
#include "project/fl_history.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "media/fl_speech_align.h"
#include "schema/fl_widgets.h"
#include "time/fl_scene_state.h"
#include "time/fl_time_mapping.h"
#include "time/fl_composition.h"
#include "components/fl_events.h"
#include "doc/fl_document.h"
#include "agent/fl_agent_internal.h"
#include <stdlib.h>
#include <string.h>

#define SECOND ((Fl_Tick)FL_TICKS_PER_SECOND)

/* Borrow a live cue by its exact label and kind. */
static const Fl_SpeechCue *find(const Fl_Phase *phase, const char *text, Fl_SpeechCueKind kind)
{
    for (size_t i = 0; i < phase->cue_count; ++i)
        if (!phase->cues[i].retired && phase->cues[i].kind == kind && !strcmp(phase->cues[i].text,text)) return &phase->cues[i];
    return NULL;
}

/* Apply a complete measured alignment through the document owner. */
static void align_phase(Fl_Project *project, Fl_Id scene, Fl_Id phase, Fl_Id asset, const char *text, const Fl_SpeechCue *cues, size_t count)
{
    const Fl_Phase *current = fl_scene_phase(fl_project_scene(project,scene),phase);
    Fl_Id beat = current->beat, clip = current->clip;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_UPDATE, .scene = scene, .phase = phase,
        .name = "Narration", .source = text, .beat = beat, .clip = clip},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_CUES, .scene = scene, .phase = phase,
        .asset = asset, .cues = cues, .cue_count = count},NULL));
}

/* Supply the open project to the public agent surface. */
static Fl_Project *host_project(void *data) {return data;}

/* Verify recognition mismatches, repeated text and sentence timing never manufacture timestamps. */
static void check_parser(void)
{
    const char *json = "{\"transcription\":[{\"text\":\"Alpha\",\"offsets\":{\"from\":100,\"to\":300}},"
        "{\"text\":\"gamma.\",\"offsets\":{\"from\":700,\"to\":900}}]}";
    Fl_SpeechAlignment alignment = {0}; char error[256] = {0};
    CHECK(fl_speech_alignment_parse(json,"Alpha beta gamma.",SECOND,&alignment,error,sizeof(error)));
    CHECK(alignment.count == 4);
    if (alignment.count == 4) {
        CHECK(alignment.cues[0].resolved && alignment.cues[0].start == SECOND / 10);
        CHECK(!alignment.cues[1].resolved && alignment.cues[2].resolved);
        CHECK(alignment.cues[3].kind == FL_CUE_SENTENCE && !alignment.cues[3].resolved);
        CHECK(alignment.cues[3].start == 0 && alignment.cues[3].end == 0);
    }
    fl_speech_alignment_free(&alignment);
    CHECK(fl_speech_alignment_parse("{\"transcription\":[{\"text\":\"go go\",\"offsets\":{\"from\":0,\"to\":500}}]}","go go go",SECOND,&alignment,error,sizeof(error)));
    CHECK(alignment.count == 4 && !alignment.cues[0].resolved && !alignment.cues[1].resolved && !alignment.cues[2].resolved);
    CHECK(alignment.cues[0].end == 0 && alignment.cues[1].end == 0);
    fl_speech_alignment_free(&alignment);
    CHECK(fl_speech_alignment_parse("{\"transcription\":[{\"text\":\"Alpha\",\"offsets\":{\"from\":0,\"to\":500},\"tokens\":[{\"text\":\"Alpha\",\"p\":0.1}]}]}","Alpha",SECOND,&alignment,error,sizeof(error)));
    CHECK(alignment.count == 2 && !alignment.cues[0].resolved && alignment.cues[0].start == 0 && alignment.cues[0].end == SECOND / 2);
    CHECK(!alignment.cues[1].resolved && alignment.cues[1].end == SECOND / 2);
    fl_speech_alignment_free(&alignment);
    CHECK(fl_speech_alignment_parse("{\"transcription\":[{\"text\":\"Alpha\",\"offsets\":{\"from\":700,\"to\":900}},{\"text\":\"beta\",\"offsets\":{\"from\":100,\"to\":300}}]}","Alpha beta",SECOND,&alignment,error,sizeof(error)));
    CHECK(alignment.count == 3 && !alignment.cues[2].resolved && alignment.cues[2].start == 0 && alignment.cues[2].end == 0);
    fl_speech_alignment_free(&alignment);
    CHECK(fl_speech_alignment_parse("{\"transcription\":[{\"text\":\"Alpha\",\"offsets\":{\"from\":0,\"to\":1500}}]}","Alpha",SECOND,&alignment,error,sizeof(error)));
    CHECK(alignment.count == 2 && !alignment.cues[0].resolved && alignment.cues[0].end == 0);
    fl_speech_alignment_free(&alignment);
    CHECK(!fl_speech_alignment_parse("{}","Alpha",SECOND,&alignment,error,sizeof(error)));
    CHECK(fl_speech_alignment_parse("{\"transcription\":[{\"text\":\"日本語。\",\"offsets\":{\"from\":0,\"to\":500}}]}",
        "日本語。",SECOND,&alignment,error,sizeof(error)));
    CHECK(alignment.count == 4 && alignment.cues[0].resolved); fl_speech_alignment_free(&alignment);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); if (argc != 2) return 2;
    check_parser();
    Fl_Project *project = fl_project_create(argv[1],"Speech cues",NULL);
    CHECK(project); if (!project) return 1;
    Fl_Id scene = fl_project_scene_at(project,0)->id, phase = 0, clip = 0, asset = 0, entity = 0, key = 0;
    Fl_Asset record = {.name = "Narration", .kind = FL_ASSET_AUDIO, .hash = {17}, .extension = "wav",
        .size = 16, .duration = 12 * SECOND, .sample_rate = 24000, .channels = 1};
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &record},&asset));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_ADD, .scene = scene, .name = "Narration", .source = "Alpha beta gamma."},&phase));
    Fl_Id beat = fl_scene_phase(fl_project_scene(project,scene),phase)->beat;
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = scene, .beat = beat, .name = "Narration", .beat_duration = 12 * SECOND},NULL));
    Fl_AudioClip audio = {.asset = asset, .start = {.kind = FL_ANCHOR_BEAT, .beat = beat}, .gain = 1};
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_CLIP_ADD, .scene = scene, .clip_record = &audio},&clip));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_UPDATE, .scene = scene, .phase = phase, .name = "Narration",
        .source = "Alpha beta gamma.", .beat = beat, .clip = clip},NULL));
    Fl_SpeechCue first[] = {
        {.text = "Alpha", .start = SECOND, .end = SECOND + SECOND / 2, .resolved = true},
        {.text = "beta", .start = 2 * SECOND, .end = 2 * SECOND + SECOND / 2, .resolved = true},
        {.text = "gamma", .start = 3 * SECOND, .end = 3 * SECOND + SECOND / 2, .resolved = true},
        {.text = "Alpha beta gamma", .kind = FL_CUE_SENTENCE, .start = SECOND, .end = 3 * SECOND + SECOND / 2, .resolved = true}};
    align_phase(project,scene,phase,asset,"Alpha beta gamma.",first,4);
    const Fl_Phase *current = fl_scene_phase(fl_project_scene(project,scene),phase);
    Fl_Id beta = find(current,"beta",FL_CUE_WORD)->id, gamma = find(current,"gamma",FL_CUE_WORD)->id;
    Fl_TimeAnchor at = {.kind = FL_ANCHOR_SPEECH, .cue = beta, .offset = SECOND / 4}; Fl_Tick tick;
    CHECK(fl_anchor_sample(fl_project_scene(project,scene),&at,&tick) && tick == 2 * SECOND + SECOND / 4);
    CHECK(fl_widget_create(project,scene,0,FL_WIDGET_RECTANGLE,0,0,&entity));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene, .entity = entity, .property = FL_PROP_X,
        .anchor = at, .value = {42}, .ease = FL_EASE_CUBIC, .ease_param = {0.1,0.2,0.8,0.9}},&key));
    Fl_SpeechCue revised[] = {
        {.text = "Alpha", .start = SECOND, .end = SECOND + SECOND / 2, .resolved = true},
        {.text = "new", .start = 3 * SECOND, .end = 3 * SECOND + SECOND / 2, .resolved = true},
        {.text = "beta", .start = 5 * SECOND, .end = 5 * SECOND + SECOND / 2, .resolved = true},
        {.text = "gamma", .start = 7 * SECOND, .end = 7 * SECOND + SECOND / 2, .resolved = true}};
    CHECK(fl_project_edit_begin(project,"Regenerate measured narration",false));
    align_phase(project,scene,phase,asset,"Alpha new beta gamma.",revised,4);
    CHECK(fl_project_edit_commit(project));
    current = fl_scene_phase(fl_project_scene(project,scene),phase);
    CHECK(find(current,"beta",FL_CUE_WORD)->id == beta && find(current,"gamma",FL_CUE_WORD)->id == gamma);
    const Fl_Keyframe *bound = fl_scene_key(fl_project_scene(project,scene),key,NULL);
    CHECK(bound && bound->value[0] == 42 && bound->ease == FL_EASE_CUBIC && bound->ease_param[3] == 0.9);
    CHECK(fl_anchor_sample(fl_project_scene(project,scene),&bound->at,&tick) && tick == 5 * SECOND + SECOND / 4);
    double value[4]; CHECK(fl_track_sample(fl_project_scene(project,scene),fl_scene_track(fl_project_scene(project,scene),entity,FL_PROP_X),tick,value) && value[0] == 42);
    char label[FL_HISTORY_LABEL_MAX]; bool conflict = false;
    CHECK(fl_history_undo(fl_project_history(project),project,label,&conflict) && !conflict);
    CHECK(fl_anchor_sample(fl_project_scene(project,scene),&at,&tick) && tick == 2 * SECOND + SECOND / 4);
    CHECK(fl_history_redo(fl_project_history(project),project,label,&conflict) && !conflict);
    Fl_SpeechCue removed[] = {revised[0],revised[1],revised[3]};
    align_phase(project,scene,phase,asset,"Alpha new gamma.",removed,3);
    CHECK(!fl_anchor_sample(fl_project_scene(project,scene),&at,&tick));
    CHECK(fl_scene_speech_cue(fl_project_scene(project,scene),beta,NULL)->retired);
    bound = fl_scene_key(fl_project_scene(project,scene),key,NULL); CHECK(bound && bound->at.cue == beta && bound->value[0] == 42);
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_KEY_SET, .scene = scene, .entity = entity, .key = key, .property = FL_PROP_X,
        .anchor = {.kind = FL_ANCHOR_SPEECH, .cue = gamma, .offset = SECOND / 4}, .value = {42}, .ease = FL_EASE_CUBIC, .ease_param = {0.1,0.2,0.8,0.9}},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_CUE_UPDATE, .scene = scene, .cue = gamma, .cue_start = 8 * SECOND, .cue_end = 9 * SECOND},NULL));
    bound = fl_scene_key(fl_project_scene(project,scene),key,NULL);
    CHECK(fl_anchor_sample(fl_project_scene(project,scene),&bound->at,&tick) && tick == 8 * SECOND + SECOND / 4);
    Fl_TimeMapping mapping = fl_time_mapping_default(); mapping.explicit_range = true; mapping.duration = SECOND;
    mapping.start = (Fl_TimeAnchor){.kind = FL_ANCHOR_SPEECH, .cue = gamma};
    mapping.end = (Fl_TimeAnchor){.kind = FL_ANCHOR_SPEECH, .cue = gamma, .end = true};
    Fl_TimeSample local; CHECK(fl_time_mapping_sample(fl_project_scene(project,scene),&mapping,8 * SECOND + SECOND / 2,&local));
    CHECK(local.active && local.tick == SECOND / 2);
    char composition_error[256];
    Fl_Composition *composition = fl_composition_create(fl_project_scene(project,scene),NULL,composition_error,sizeof(composition_error));
    CHECK(composition);
    if (composition) {
        const Fl_Scene *flat = fl_composition_scene(composition);
        CHECK(flat->phases != fl_project_scene(project,scene)->phases);
        CHECK(flat->phases[0].cues != fl_project_scene(project,scene)->phases[0].cues);
        CHECK(fl_anchor_sample(flat,&bound->at,&tick) && tick == 8 * SECOND + SECOND / 4);
        fl_composition_destroy(composition);
    }
    Fl_Json context = {0}; cJSON *json = fl_document_anchor_write(&context,&bound->at); Fl_TimeAnchor decoded = {0};
    CHECK(fl_document_anchor_read(&context,json,&decoded) && decoded.cue == gamma); cJSON_Delete(json);
    cJSON *event_document = cJSON_CreateObject(), *points = cJSON_AddArrayToObject(event_document,"points"), *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event_document,"next_id","2"); cJSON_AddStringToObject(event,"id","1");
    cJSON_AddItemToObject(event,"at",fl_document_anchor_write(&context,&bound->at));
    cJSON_AddStringToObject(event,"name","reveal"); cJSON_AddNumberToObject(event,"value",1); cJSON_AddItemToArray(points,event);
    Fl_Component events = {0}; CHECK(fl_events_encode(event_document,&events)); fl_component_clear(&events); cJSON_Delete(event_document);
    Fl_Fail fail = {0}; char request[160]; snprintf(request,sizeof(request),"{\"cue\":\"%llu\",\"edge\":\"end\",\"offset\":-0.2}",(unsigned long long)gamma);
    json = cJSON_Parse(request); CHECK(agent_parse_anchor(json,NULL,&decoded,&fail,"at") && decoded.end && decoded.offset == -SECOND / 5); cJSON_Delete(json);
    Fl_Agent *agent = fl_agent_create(&(Fl_AgentHost){.project = host_project, .user_data = project}); CHECK(agent);
    char operation[700]; snprintf(operation,sizeof(operation),
        "{\"ops\":[{\"op\":\"key.set\",\"entity\":\"%llu\",\"key\":\"%llu\",\"property\":\"x\",\"at\":{\"cue\":\"%llu\",\"offset\":0.25},\"value\":42,\"ease\":\"cubic\",\"curve\":[0.1,0.2,0.8,0.9]},{\"op\":\"cue.set\",\"scene\":\"%llu\",\"cue\":\"%llu\",\"start\":8,\"end\":9}]}",
        (unsigned long long)entity,(unsigned long long)key,(unsigned long long)gamma,(unsigned long long)scene,(unsigned long long)gamma);
    cJSON *arguments = cJSON_Parse(operation), *error = NULL;
    cJSON *result = fl_agent_call(agent,"apply_ops",arguments,&error); CHECK(result && !error);
    cJSON_Delete(arguments); cJSON_Delete(result); cJSON_Delete(error); fl_agent_destroy(agent);
    CHECK(fl_project_save(project)); fl_project_destroy(project); project = fl_project_open(argv[1],NULL); CHECK(project);
    if (!project) return 1;
    bound = fl_scene_key(fl_project_scene(project,scene),key,NULL);
    CHECK(bound && bound->at.cue == gamma && bound->value[0] == 42);
    CHECK(fl_anchor_sample(fl_project_scene(project,scene),&bound->at,&tick) && tick == 8 * SECOND + SECOND / 4);
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind = FL_OP_CUE_UPDATE, .scene = scene, .cue = gamma, .cue_start = -1, .cue_end = SECOND},NULL));
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = asset},NULL));
    Fl_SpeechCue repeated[] = {{.text = "go", .start = SECOND, .end = 2 * SECOND, .resolved = true},
        {.text = "go", .start = 3 * SECOND, .end = 4 * SECOND, .resolved = true},
        {.text = "go", .start = 5 * SECOND, .end = 6 * SECOND, .resolved = true}};
    align_phase(project,scene,phase,asset,"go go",repeated,2);
    current = fl_scene_phase(fl_project_scene(project,scene),phase);
    Fl_Id repeated_a = current->cues[0].id, repeated_b = current->cues[1].id;
    align_phase(project,scene,phase,asset,"go go go",repeated,3);
    CHECK(fl_scene_speech_cue(fl_project_scene(project,scene),repeated_a,NULL)->retired);
    CHECK(fl_scene_speech_cue(fl_project_scene(project,scene),repeated_b,NULL)->retired);
    current = fl_scene_phase(fl_project_scene(project,scene),phase);
    CHECK(current->cues[0].id != repeated_a && current->cues[0].id != repeated_b);
    repeated[0].end = 13 * SECOND;
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind = FL_OP_PHASE_CUES, .scene = scene, .phase = phase, .asset = asset, .cues = repeated, .cue_count = 1},NULL));
    Fl_Id pending = fl_scene_phase(fl_project_scene(project,scene),phase)->cues[0].id;
    CHECK(fl_project_save(project)); fl_project_destroy(project);
    char path[FL_PATH_MAX], file[96]; snprintf(file,sizeof(file),"scenes/%016llx.scene.json",(unsigned long long)scene);
    CHECK(fl_fs_join(path,sizeof(path),argv[1],file));
    char *document_text = fl_fs_read_file(path,16 * 1024 * 1024,NULL); CHECK(document_text);
    cJSON *document = document_text ? cJSON_Parse(document_text) : NULL; free(document_text); CHECK(document);
    cJSON *stored_phase = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(document,"phases"),0);
    cJSON *stored_cue = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(stored_phase,"cues"),0);
    char beyond[32]; snprintf(beyond,sizeof(beyond),"%lld",(long long)(12 * SECOND + 19200));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(stored_cue,"end",cJSON_CreateString(beyond)));
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(stored_cue,"resolved",cJSON_CreateFalse()));
    document_text = cJSON_PrintUnformatted(document); cJSON_Delete(document);
    CHECK(document_text && fl_fs_write_atomic(path,document_text,strlen(document_text))); free(document_text);
    project = fl_project_open(argv[1],NULL); CHECK(project);
    if (!project) return FLOW_TEST_RESULT("fl_speech_cues_tests");
    const Fl_SpeechCue *evidence = fl_scene_speech_cue(fl_project_scene(project,scene),pending,NULL);
    CHECK(evidence && !evidence->resolved && evidence->end == 12 * SECOND + 19200);
    CHECK(!fl_speech_cue_sample(fl_project_scene(project,scene),pending,false,&tick));
    CHECK(!fl_project_apply(project,&(Fl_Op){.kind = FL_OP_CUE_UPDATE, .scene = scene, .cue = pending,
        .cue_start = SECOND, .cue_end = 12 * SECOND + 19200},NULL));
    CHECK(fl_project_apply(project,&(Fl_Op){.kind = FL_OP_CUE_UPDATE, .scene = scene, .cue = pending,
        .cue_start = SECOND, .cue_end = 2 * SECOND},NULL));
    CHECK(fl_speech_cue_sample(fl_project_scene(project,scene),pending,false,&tick) && tick == SECOND);
    CHECK(fl_history_undo(fl_project_history(project),project,NULL,NULL));
    CHECK(!fl_speech_cue_sample(fl_project_scene(project,scene),pending,false,&tick));
    CHECK(fl_project_save(project)); fl_project_destroy(project);
    project = fl_project_open(argv[1],NULL); CHECK(project);
    if (project) CHECK(!fl_speech_cue_sample(fl_project_scene(project,scene),pending,false,&tick));
    fl_project_destroy(project);
    return FLOW_TEST_RESULT("fl_speech_cues_tests");
}

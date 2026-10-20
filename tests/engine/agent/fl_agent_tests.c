#include "flow_test.h"
#include "agent/fl_agent.h"
#include "project/fl_history.h"
#include "project/fl_prototype.h"
#include "agent/fl_mcp.h"
#include "agent/fl_mcp_http.h"
#include "core/fl_thread.h"
#include "media/fl_png.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <civetweb.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static Fl_Project *g_project;

/* Stand-in for the editor's GPU renderer: a gradient of the requested size, so the tool plumbing is testable headless. */
static bool fake_render(void *user, const Fl_Project *project, const Fl_Scene *scene, Fl_Tick tick, uint32_t width, Fl_Frame *out)
{
    (void)user; (void)project; (void)tick;
    uint32_t w, h;
    if (!fl_frame_dimensions(scene, width, &w, &h)) return false;
    out->rgba = malloc((size_t)w * h * 4);
    if (!out->rgba) return false;
    for (size_t i = 0; i < (size_t)w * h; ++i) {out->rgba[i * 4] = (unsigned char)i; out->rgba[i * 4 + 1] = 40; out->rgba[i * 4 + 2] = 90; out->rgba[i * 4 + 3] = 255;}
    out->width = w; out->height = h;
    return true;
}
static Fl_Project *host_project(void *user) {(void)user; return g_project;}

/* Parse literal JSON text; the tests build every request this way. */
static cJSON *json(const char *text)
{
    cJSON *value = cJSON_Parse(text);
    CHECK(value != NULL);
    return value;
}

/* Call a tool with JSON arguments; returns the result (NULL on error, with *error set). */
static cJSON *call(Fl_Agent *agent, const char *tool, const char *arguments, cJSON **error)
{
    cJSON *args = arguments ? json(arguments) : NULL;
    cJSON *result = fl_agent_call(agent, tool, args, error);
    cJSON_Delete(args);
    return result;
}

static double number(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    CHECK(cJSON_IsNumber(item));
    return cJSON_IsNumber(item) ? item->valuedouble : NAN;
}

static bool error_code(const cJSON *error, const char *code)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(error, "code");
    return cJSON_IsString(item) && !strcmp(item->valuestring, code);
}

/* Find an entity in a get_scene result by name. */
static const cJSON *entity_named(const cJSON *scene, const char *name)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(scene, "entity_list"), *item;
    cJSON_ArrayForEach(item, list) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (cJSON_IsString(n) && !strcmp(n->valuestring, name)) return item;
    }
    return NULL;
}

static size_t entity_count(Fl_Id scene) {return fl_project_scene(g_project, scene)->entity_count;}

/* Operations build content, validate atomically, support expectations, dry runs and undo/redo. */
static void test_authoring(Fl_Agent *agent, Fl_Id scene)
{
    cJSON *error = NULL, *result;
    char request[2048];
    snprintf(request, sizeof(request),
        "{\"label\":\"Intro\",\"ops\":[{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"rectangle\",\"as\":\"$box\",\"props\":{\"name\":\"Box\",\"x\":100,\"y\":50,\"width\":200,\"height\":100,\"fill\":\"#ff0000\",\"rotation\":90}},{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"text\",\"parent\":\"$box\",\"as\":\"$t\",\"props\":{\"name\":\"Title\",\"text\":\"Hello\",\"font_size\":64}},{\"op\":\"key.set\",\"entity\":\"$box\",\"property\":\"x\",\"at\":0,\"value\":100},{\"op\":\"key.set\",\"entity\":\"$box\",\"property\":\"x\",\"at\":2,\"value\":600,\"ease\":\"in_out\"},{\"op\":\"script.create\",\"name\":\"Drift\",\"source\":\"return { sample = function(ctx) local self=ctx.entity; local time=ctx.time.seconds;  self.transform.y = self.transform.y + 10 end }\",\"as\":\"$s\"},{\"op\":\"script.attach\",\"entity\":\"$t\",\"script\":\"$s\"}]}", (unsigned long long)scene, (unsigned long long)scene);
    cJSON *args = json(request);
    result = fl_agent_call(agent, "apply_ops", args, &error);
    cJSON_Delete(args);
    CHECK(result && !error);
    if (!result) {char *text = error ? cJSON_Print(error) : NULL; if (text) fprintf(stderr, "%s\n", text); free(text); return;}
    CHECK(number(result, "applied") == 6 && number(result, "undo_available") == 1);
    cJSON_Delete(result);
    CHECK(entity_count(scene) == 2);

    char scene_args[64];
    snprintf(scene_args, sizeof(scene_args), "{\"scene\":%llu,\"include_scripts\":true}", (unsigned long long)scene);
    result = call(agent, "get_scene", scene_args, &error);
    CHECK(result != NULL);
    const cJSON *box = entity_named(result, "Box"), *title = entity_named(result, "Title");
    CHECK(box && title);
    CHECK(fabs(number(box, "rotation") - 90) < 1e-9 && number(box, "width") == 200);
    const cJSON *fill = cJSON_GetObjectItemCaseSensitive(box, "fill");
    CHECK(cJSON_IsString(fill) && !strcmp(fill->valuestring, "#ff0000ff"));
    const cJSON *tracks = cJSON_GetObjectItemCaseSensitive(box, "tracks");
    CHECK(cJSON_GetArraySize(tracks) == 1 && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(tracks->child, "keys")) == 2);
    CHECK(number(title, "parent") == number(box, "id") && !strcmp(cJSON_GetObjectItemCaseSensitive(title, "text")->valuestring, "Hello"));
    const cJSON *script = cJSON_GetObjectItemCaseSensitive(title, "script");
    CHECK(script && !strcmp(cJSON_GetObjectItemCaseSensitive(script, "name")->valuestring, "Drift") && number(script, "id") > 0);
    const cJSON *repository = cJSON_GetObjectItemCaseSensitive(result, "scripts");
    CHECK(cJSON_GetArraySize(repository) == 1 && cJSON_GetObjectItemCaseSensitive(repository->child, "source") &&
          cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(repository->child, "used_by")) == 1);
    double revision = number(result, "revision"), box_id = number(box, "id");
    cJSON_Delete(result);

    /* Evaluation applies keyframes (linear towards the in_out key) and scripts. */
    snprintf(scene_args, sizeof(scene_args), "{\"scene\":%llu,\"time\":1}", (unsigned long long)scene);
    result = call(agent, "evaluate_scene", scene_args, &error);
    CHECK(result != NULL);
    if (result) {
        const cJSON *list = cJSON_GetObjectItemCaseSensitive(result, "entities");
        CHECK(fabs(number(list->child, "x") - 350) < 1e-6);
        CHECK(!cJSON_GetObjectItemCaseSensitive(list->child->next, "script_error"));
    }
    cJSON_Delete(result);

    /* A failing operation applies nothing, and names the failing index. */
    snprintf(request, sizeof(request),
        "{\"ops\":[{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"ellipse\"},{\"op\":\"key.set\",\"entity\":\"$missing\",\"property\":\"x\",\"at\":0,\"value\":1}]}",
        (unsigned long long)scene);
    result = call(agent, "apply_ops", request, &error);
    CHECK(!result && error && error_code(error, "unknown_reference") && number(error, "op_index") == 1);
    cJSON_Delete(error); error = NULL;
    CHECK(entity_count(scene) == 2);
    snprintf(request, sizeof(request),
        "{\"ops\":[{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"ellipse\"},{\"op\":\"key.set\",\"entity\":%.0f,\"property\":\"x\",\"at\":99,\"value\":1}]}",
        (unsigned long long)scene, box_id);
    result = call(agent, "apply_ops", request, &error);
    CHECK(!result && error && error_code(error, "rejected"));
    cJSON_Delete(error); error = NULL;
    CHECK(entity_count(scene) == 2);

    /* Typos and inapplicable properties are rejected, not ignored. */
    snprintf(request, sizeof(request), "{\"ops\":[{\"op\":\"entity.set\",\"entity\":%.0f,\"props\":{\"colour\":\"#fff\"}}]}", box_id);
    result = call(agent, "apply_ops", request, &error);
    CHECK(!result && error_code(error, "unknown_field")); cJSON_Delete(error); error = NULL;
    snprintf(request, sizeof(request), "{\"ops\":[{\"op\":\"entity.set\",\"entity\":%.0f,\"props\":{\"text\":\"x\"}}]}", box_id);
    result = call(agent, "apply_ops", request, &error);
    CHECK(!result && error_code(error, "not_applicable")); cJSON_Delete(error); error = NULL;

    /* Stale expectations refuse the batch before touching anything. */
    snprintf(request, sizeof(request), "{\"expect\":{\"%llu\":%.0f},\"ops\":[{\"op\":\"scene.rename\",\"scene\":%llu,\"name\":\"X\"}]}",
             (unsigned long long)scene, revision - 1, (unsigned long long)scene);
    result = call(agent, "apply_ops", request, &error);
    CHECK(!result && error_code(error, "revision_conflict") && cJSON_GetObjectItemCaseSensitive(error, "changed_scenes"));
    cJSON_Delete(error); error = NULL;

    /* Dry runs validate and report ids but leave the document alone. */
    snprintf(request, sizeof(request), "{\"dry_run\":true,\"ops\":[{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"line\",\"as\":\"$l\"}]}", (unsigned long long)scene);
    result = call(agent, "apply_ops", request, &error);
    CHECK(result && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "dry_run")) && entity_count(scene) == 2);
    cJSON_Delete(result);

    /* Whole-changeset undo/redo. */
    result = call(agent, "undo", NULL, &error);
    CHECK(result && entity_count(scene) == 0);
    cJSON_Delete(result);
    result = call(agent, "redo", NULL, &error);
    CHECK(result && entity_count(scene) == 2);
    cJSON_Delete(result);
    result = call(agent, "redo", NULL, &error);
    CHECK(!result && error_code(error, "nothing_to_do")); cJSON_Delete(error); error = NULL;

    /* Editor and MCP edits share history: undo the newest human edit, then redo it. */
    CHECK(fl_project_apply(g_project, &(Fl_Op){.kind = FL_OP_SCENE_RENAME, .scene = scene, .name = "Edited by hand"}, NULL));
    result = call(agent, "undo", NULL, &error);
    CHECK(result && !error && entity_count(scene) == 2 && strcmp(fl_project_scene(g_project,scene)->name,"Edited by hand"));
    cJSON_Delete(result);
    result=call(agent,"redo",NULL,&error);
    CHECK(result && !error && !strcmp(fl_project_scene(g_project,scene)->name,"Edited by hand"));
    cJSON_Delete(result);
}

/* Look properties apply only where they make sense, validate their ranges and round-trip through get_scene. */
static void test_look(Fl_Agent *agent, Fl_Id scene)
{
    cJSON *error = NULL, *result;
    Fl_Asset movie = {.name = "Clip", .kind = FL_ASSET_VIDEO, .hash = {9}, .extension = "mp4", .size = 10, .width = 1920, .height = 1080, .duration = 10 * FL_TICKS_PER_SECOND};
    Fl_Asset picture = {.name = "Still", .kind = FL_ASSET_IMAGE, .hash = {8}, .extension = "png", .size = 10, .width = 200, .height = 100};
    Fl_Id movie_id, picture_id;
    CHECK(fl_project_apply(g_project, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &movie}, &movie_id));
    CHECK(fl_project_apply(g_project, &(Fl_Op){.kind = FL_OP_ASSET_ADD, .asset_record = &picture}, &picture_id));
    char request[2048];
    snprintf(request, sizeof(request),
        "{\"ops\":[{\"op\":\"scene.set\",\"scene\":%llu,\"background\":\"#102030\"},"
        "{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"rectangle\",\"props\":{\"name\":\"Card\",\"radius\":20,\"fill2\":\"#ff0000\",\"gradient_angle\":90,\"shadow_blur\":30,\"shadow_y\":10}},"
        "{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"text\",\"props\":{\"name\":\"Caption\",\"align\":\"center\"}},"
        "{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"video\",\"asset\":%llu,\"props\":{\"name\":\"Footage\",\"start\":2,\"offset\":1,\"rate\":2,\"loop\":true,\"radius\":12,\"crop\":[0,0,0.5,0.5]}},"
        "{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"image\",\"asset\":%llu,\"as\":\"$still\"},"
        "{\"op\":\"key.set\",\"entity\":\"$still\",\"property\":\"crop_w\",\"at\":0,\"value\":1},"
        "{\"op\":\"key.set\",\"entity\":\"$still\",\"property\":\"crop_w\",\"at\":2,\"value\":0.5}]}",
        (unsigned long long)scene, (unsigned long long)scene, (unsigned long long)scene, (unsigned long long)scene,
        (unsigned long long)movie_id, (unsigned long long)scene, (unsigned long long)picture_id);
    result = call(agent, "apply_ops", request, &error);
    CHECK(result != NULL);
    if (!result) {char *text = error ? cJSON_Print(error) : NULL; if (text) fprintf(stderr, "%s\n", text); free(text); cJSON_Delete(error); return;}
    cJSON_Delete(result);
    char scene_args[64];
    snprintf(scene_args, sizeof(scene_args), "{\"scene\":%llu}", (unsigned long long)scene);
    result = call(agent, "get_scene", scene_args, &error);
    CHECK(result != NULL);
    const cJSON *card = entity_named(result, "Card"), *caption = entity_named(result, "Caption"), *footage = entity_named(result, "Footage");
    CHECK(card && caption && footage);
    if (card && caption && footage) {
        CHECK(number(card, "radius") == 20 && number(card, "shadow_blur") == 30 && fabs(number(card, "gradient_angle") - 90) < 1e-9);
        CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(card, "fill2")->valuestring, "#ff0000ff"));
        CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(caption, "align")->valuestring, "center"));
        CHECK(number(footage, "start") == 2 && number(footage, "offset") == 1 && number(footage, "rate") == 2 && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(footage, "loop")));
        CHECK(number(footage, "asset") == (double)movie_id && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(footage, "crop")) == 4);
        CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(result, "background")->valuestring, "#102030ff"));
    }
    cJSON_Delete(result);
    const char *rejected[][2] = {
        {"{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"text\",\"props\":{\"radius\":5}}", "not_applicable"},
        {"{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"rectangle\",\"props\":{\"align\":\"center\"}}", "not_applicable"},
        {"{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"rectangle\",\"props\":{\"start\":1}}", "not_applicable"},
        {"{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"rectangle\",\"props\":{\"crop\":[0,0,1,1]}}", "not_applicable"},
        {"{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"image\",\"asset\":%llu,\"props\":{\"crop\":[0.5,0,0.8,1]}}", "invalid_argument"},
        {"{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"video\",\"asset\":%llu}", "not_found"},
        {"{\"op\":\"scene.set\",\"scene\":%llu,\"background\":\"#10203080\"}", "invalid_argument"},
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(*rejected); ++i) {
        char op[512];
        snprintf(op, sizeof(op), rejected[i][0], (unsigned long long)scene, (unsigned long long)picture_id);
        snprintf(request, sizeof(request), "{\"ops\":[%s]}", op);
        result = call(agent, "apply_ops", request, &error);
        CHECK(!result && error && error_code(error, rejected[i][1]));
        cJSON_Delete(error); error = NULL;
    }
}

/* Scene-list changes undo as whole changesets and the restored project saves and reopens. */
/* The script tools: upsert by name, patch with edits, share across entities, report syntax errors, and undo as one step. */
static void test_scripts(Fl_Agent *agent, Fl_Id scene)
{
    cJSON *error = NULL, *result;
    char request[1024];
    snprintf(request, sizeof(request), "{\"ops\":[{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"rectangle\",\"as\":\"$a\"},"
             "{\"op\":\"entity.create\",\"scene\":%llu,\"kind\":\"rectangle\",\"as\":\"$b\"}]}", (unsigned long long)scene, (unsigned long long)scene);
    result = call(agent, "apply_ops", request, &error);
    CHECK(result != NULL);
    const cJSON *ids = result ? cJSON_GetObjectItemCaseSensitive(result, "results") : NULL;
    const double first = ids ? number(ids->child, "id") : 0, second = ids ? number(ids->child->next, "id") : 0;
    cJSON_Delete(result);

    snprintf(request, sizeof(request), "{\"script\":\"Wobble\",\"source\":\"return { sample = function(ctx) local self=ctx.entity; local t=ctx.time.seconds;  self.transform.x = 10 * t end }\",\"attach\":[%.0f,%.0f]}",
             first, second);
    result = call(agent, "set_script", request, &error);
    CHECK(result && !error);
    cJSON_Delete(result);
    snprintf(request, sizeof(request), "{\"script\":\"Wobble\"}");
    result = call(agent, "get_script", request, &error);
    const cJSON *script = result ? cJSON_GetObjectItemCaseSensitive(result, "script") : NULL;
    CHECK(script && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(script, "used_by")) == 2 &&
          strstr(cJSON_GetObjectItemCaseSensitive(script, "source")->valuestring, "10 * t"));
    const double id = script ? number(script, "id") : 0;
    cJSON_Delete(result);

    snprintf(request, sizeof(request), "{\"script\":%.0f,\"edits\":[{\"old\":\"10 * t\",\"new\":\"25 * t\"}]}", id);
    result = call(agent, "set_script", request, &error);
    CHECK(result && !error);
    cJSON_Delete(result);
    snprintf(request, sizeof(request), "{\"scene\":%llu,\"time\":2}", (unsigned long long)scene);
    result = call(agent, "evaluate_scene", request, &error);
    const cJSON *list = result ? cJSON_GetObjectItemCaseSensitive(result, "entities") : NULL, *item;
    size_t checked = 0;
    cJSON_ArrayForEach(item, list) if (number(item, "id") == first || number(item, "id") == second) {CHECK(number(item, "x") == 50); ++checked;}
    CHECK(checked == 2);
    cJSON_Delete(result);

    snprintf(request, sizeof(request), "{\"script\":%.0f,\"edits\":[{\"old\":\"missing\",\"new\":\"x\"}]}", id);
    result = call(agent, "set_script", request, &error);
    CHECK(!result && error && error_code(error, "invalid_argument"));
    cJSON_Delete(error); error = NULL;
    snprintf(request, sizeof(request), "{\"script\":%.0f,\"edits\":[{\"old\":\"t\",\"new\":\"x\"}]}", id);
    result = call(agent, "set_script", request, &error);
    CHECK(!result && error);
    cJSON_Delete(error); error = NULL;

    snprintf(request, sizeof(request), "{\"script\":\"Wobble\",\"source\":\"function on_evaluate(\"}");
    result = call(agent, "set_script", request, &error);
    CHECK(!result && error && error_code(error,"rejected"));
    cJSON_Delete(error); error=NULL;

    snprintf(request, sizeof(request), "{\"script\":\"Nope\"}");
    result = call(agent, "set_script", request, &error);
    CHECK(!result && error && error_code(error, "not_found"));
    cJSON_Delete(error); error = NULL;
    snprintf(request, sizeof(request), "{}");
    result = call(agent, "get_script", request, &error);
    const cJSON *scripts = result ? cJSON_GetObjectItemCaseSensitive(result, "scripts") : NULL;
    CHECK(scripts && cJSON_GetArraySize(scripts) == 2 && !cJSON_GetObjectItemCaseSensitive(scripts->child, "source"));
    cJSON_Delete(result);

    snprintf(request,sizeof(request),"{\"dry_run\":true,\"ops\":[{\"op\":\"script.update\",\"script\":%.0f,\"source\":\"return {sample=123}\"}]}",id);
    result=call(agent,"apply_ops",request,&error);
    CHECK(!result && error && error_code(error,"rejected")); cJSON_Delete(error); error=NULL;
    CHECK(strstr(fl_project_asset(g_project,(Fl_Id)id)->source,"25 * t"));
    snprintf(request, sizeof(request), "{\"ops\":[{\"op\":\"script.remove\",\"script\":%.0f}]}", id);
    result = call(agent, "apply_ops", request, &error);
    CHECK(!result && error && error_code(error,"rejected"));
    cJSON_Delete(error); error=NULL;
    CHECK(fl_project_asset(g_project,(Fl_Id)id) && fl_scene_script_uses(fl_project_scene(g_project,scene),(Fl_Id)id)==2);
    snprintf(request,sizeof(request),"{\"ops\":[{\"op\":\"script.detach\",\"entity\":%.0f},{\"op\":\"script.detach\",\"entity\":%.0f},{\"op\":\"script.remove\",\"script\":%.0f}]}",first,second,id);
    result=call(agent,"apply_ops",request,&error);
    CHECK(result && !fl_project_asset(g_project,(Fl_Id)id) && fl_scene_script_uses(fl_project_scene(g_project,scene),(Fl_Id)id)==0);
    cJSON_Delete(result);
    result = call(agent, "undo", "{}", &error);
    CHECK(result && fl_project_asset(g_project, (Fl_Id)id) && fl_scene_script_uses(fl_project_scene(g_project, scene), (Fl_Id)id) == 2);
    cJSON_Delete(result);
}

static void test_scene_history(Fl_Agent *agent, const char *directory)
{
    cJSON *error = NULL, *result;
    size_t scenes = fl_project_scene_count(g_project);
    result = call(agent, "apply_ops", "{\"ops\":[{\"op\":\"scene.add\",\"name\":\"Second\",\"as\":\"$s\"},"
        "{\"op\":\"entity.create\",\"scene\":\"$s\",\"kind\":\"ellipse\",\"props\":{\"name\":\"Dot\"}}]}", &error);
    CHECK(result != NULL); cJSON_Delete(result);
    CHECK(fl_project_scene_count(g_project) == scenes + 1);
    CHECK(fl_project_save(g_project));
    result = call(agent, "undo", NULL, &error);
    CHECK(result && fl_project_scene_count(g_project) == scenes); cJSON_Delete(result);
    CHECK(fl_project_save(g_project));
    Fl_Id first = fl_project_scene_at(g_project, 0)->id;
    char request[256];
    snprintf(request, sizeof(request), "{\"ops\":[{\"op\":\"scene.remove\",\"scene\":%llu}]}", (unsigned long long)first);
    size_t entities = entity_count(first);
    result = call(agent, "apply_ops", request, &error);
    CHECK(result != NULL); cJSON_Delete(result);
    CHECK(fl_project_scene_count(g_project) == scenes - 1 && fl_project_save(g_project));
    result = call(agent, "undo", NULL, &error);
    CHECK(result && fl_project_scene_count(g_project) == scenes && entity_count(first) == entities); cJSON_Delete(result);
    CHECK(fl_project_save(g_project));
    fl_project_destroy(g_project);
    g_project = fl_project_open(directory, NULL);
    CHECK(g_project && fl_project_scene_count(g_project) == scenes && fl_project_scene(g_project, first) && entity_count(first) == entities);
}

/* The MCP core dispatches JSON-RPC, validates input and reports tool failures as results. */
static bool inline_post(void (*task)(void *), void *data, void *user) {(void)user; task(data); return true;}

static void test_protocol(Fl_Agent *agent)
{
    Fl_Mcp *mcp = fl_mcp_create(&(Fl_McpDesc){"flow-test", "1", fl_agent_instructions(), inline_post, NULL, 2000});
    CHECK(mcp && fl_agent_register(agent, mcp));
    if (!mcp) return;
    const char *init = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-03-26\"}}";
    char *reply = fl_mcp_handle(mcp, init, strlen(init));
    cJSON *r = json(reply ? reply : "null");
    CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "result"), "protocolVersion")->valuestring, "2025-03-26"));
    cJSON_Delete(r); free(reply);
    const char *note = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    CHECK(fl_mcp_handle(mcp, note, strlen(note)) == NULL);
    const char *list = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}";
    reply = fl_mcp_handle(mcp, list, strlen(list));
    r = json(reply ? reply : "null");
    const cJSON *tools = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "result"), "tools");
    CHECK(cJSON_GetArraySize(tools) >= 8);
    cJSON_Delete(r); free(reply);
    const char *call_ok = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"get_project\",\"arguments\":{}}}";
    reply = fl_mcp_handle(mcp, call_ok, strlen(call_ok));
    r = json(reply ? reply : "null");
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(r, "result");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(res, "isError")));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(res, "content")) == 1);
    cJSON_Delete(r); free(reply);
    char render[256];
    snprintf(render, sizeof(render), "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"render_frame\",\"arguments\":{\"scene\":%llu,\"times\":[0,1],\"width\":320}}}",
             (unsigned long long)fl_project_scene_at(g_project, 0)->id);
    reply = fl_mcp_handle(mcp, render, strlen(render));
    r = json(reply ? reply : "null");
    res = cJSON_GetObjectItemCaseSensitive(r, "result");
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(res, "content"), *block;
    int images = 0;
    cJSON_ArrayForEach(block, content) images += !strcmp(cJSON_GetObjectItemCaseSensitive(block, "type")->valuestring, "image");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(res, "isError")) && images == 2 && cJSON_GetArraySize(content) == 3);
    cJSON_Delete(r); free(reply);
    const char *call_bad = "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"get_scene\",\"arguments\":{\"scene\":99999}}}";
    reply = fl_mcp_handle(mcp, call_bad, strlen(call_bad));
    r = json(reply ? reply : "null");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "result"), "isError")));
    cJSON_Delete(r); free(reply);
    const char *unknown = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"nope\"}}";
    reply = fl_mcp_handle(mcp, unknown, strlen(unknown));
    r = json(reply ? reply : "null");
    CHECK(cJSON_GetObjectItemCaseSensitive(r, "error") != NULL);
    cJSON_Delete(r); free(reply);
    reply = fl_mcp_handle(mcp, "{not json", 9);
    r = json(reply ? reply : "null");
    CHECK(number(cJSON_GetObjectItemCaseSensitive(r, "error"), "code") == -32700);
    cJSON_Delete(r); free(reply);
    const char *batch = "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]";
    reply = fl_mcp_handle(mcp, batch, strlen(batch));
    r = json(reply ? reply : "null");
    CHECK(number(cJSON_GetObjectItemCaseSensitive(r, "error"), "code") == -32600);
    cJSON_Delete(r); free(reply);
    fl_mcp_destroy(mcp);
}

/* A queue drained by the test's main thread stands in for the editor's owner thread. */
typedef struct Task {void (*run)(void *); void *data; struct Task *next;} Task;
static Task *g_queue;
static Fl_Mutex g_queue_lock;
static bool queue_post(void (*task)(void *), void *data, void *user)
{
    (void)user;
    Task *t = malloc(sizeof(*t));
    if (!t) return false;
    *t = (Task){task, data, NULL};
    fl_mutex_lock(&g_queue_lock);
    Task **tail = &g_queue;
    while (*tail) tail = &(*tail)->next;
    *tail = t;
    fl_mutex_unlock(&g_queue_lock);
    return true;
}

typedef struct Client {uint16_t port; int unauthorized, forbidden, wrong_method, ok_status, call_status; char body[4096]; volatile int done;} Client;

/* One HTTP exchange; returns the status and copies up to size-1 body bytes. */
static int exchange(uint16_t port, const char *method, const char *headers, const char *body, char *out, size_t size)
{
    char error[256], request[4096];
    snprintf(request, sizeof(request), "%s /mcp HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n%sContent-Length: %zu\r\n\r\n%s", method, (unsigned)port, headers, strlen(body), body);
    struct mg_connection *c = mg_download("127.0.0.1", port, 0, error, sizeof(error), "%s", request);
    if (!c) return -1;
    int status = mg_get_response_info(c)->status_code;
    int n = out ? mg_read(c, out, size - 1) : 0;
    if (out) out[n > 0 ? n : 0] = 0;
    mg_close_connection(c);
    return status;
}

static void client_main(void *data)
{
    Client *c = data;
    const char *auth = "Authorization: Bearer secret\r\n";
    const char *list = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
    c->unauthorized = exchange(c->port, "POST", "", list, NULL, 0);
    c->forbidden = exchange(c->port, "POST", "Authorization: Bearer secret\r\nOrigin: https://evil.example\r\n", list, NULL, 0);
    c->wrong_method = exchange(c->port, "GET", auth, "", NULL, 0);
    c->ok_status = exchange(c->port, "POST", auth, list, c->body, sizeof(c->body) - 1);
    char big[8192]; (void)big;
    c->call_status = exchange(c->port, "POST", auth, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"get_project\",\"arguments\":{}}}", c->body, sizeof(c->body));
    c->done = 1;
}

static void test_http(Fl_Agent *agent)
{
    CHECK(fl_mutex_init(&g_queue_lock));
    Fl_Mcp *mcp = fl_mcp_create(&(Fl_McpDesc){"flow-test", "1", NULL, queue_post, NULL, 5000});
    CHECK(mcp && fl_agent_register(agent, mcp));
    Fl_McpHttp *http = fl_mcp_http_start(&(Fl_McpHttpDesc){mcp, 0, "secret"});
    CHECK(http && fl_mcp_http_port(http));
    if (!http) {fl_mcp_destroy(mcp); return;}
    Client client = {.port = fl_mcp_http_port(http)};
    Fl_Thread *thread = fl_thread_create(client_main, &client);
    CHECK(thread != NULL);
    while (thread && !client.done) {
        fl_mutex_lock(&g_queue_lock);
        Task *t = g_queue; if (t) g_queue = t->next;
        fl_mutex_unlock(&g_queue_lock);
        if (t) {t->run(t->data); free(t);}
    }
    if (thread) fl_thread_join(thread);
    CHECK(client.unauthorized == 401 && client.forbidden == 403 && client.wrong_method == 405);
    CHECK(client.ok_status == 200 && client.call_status == 200 && strstr(client.body, "\"result\""));
    fl_mcp_shutdown(mcp);
    fl_mcp_http_stop(http);
    fl_mcp_destroy(mcp);
    fl_mutex_destroy(&g_queue_lock);
}

/* A tool that never answers times out instead of hanging the transport; PNG encoding crops and shrinks. */
static void silent_tool(Fl_McpCall *call, const cJSON *arguments, void *user) {(void)call; (void)arguments; (void)user;}

static void test_timeout_and_png(void)
{
    Fl_Mcp *mcp = fl_mcp_create(&(Fl_McpDesc){"flow-test", "1", NULL, inline_post, NULL, 200});
    CHECK(mcp && fl_mcp_register(mcp, &(Fl_McpTool){"silent", "never answers", "{\"type\":\"object\"}", true, silent_tool, NULL}));
    CHECK(!fl_mcp_register(mcp, &(Fl_McpTool){"silent", "duplicate", "{\"type\":\"object\"}", true, silent_tool, NULL}));
    CHECK(!fl_mcp_register(mcp, &(Fl_McpTool){"Bad Name", "x", "{\"type\":\"object\"}", true, silent_tool, NULL}));
    const char *request = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"silent\"}}";
    char *reply = fl_mcp_handle(mcp, request, strlen(request));
    CHECK(reply && strstr(reply, "timeout") && strstr(reply, "\"isError\":true"));
    free(reply);
    fl_mcp_shutdown(mcp);
    reply = fl_mcp_handle(mcp, request, strlen(request));
    CHECK(reply && strstr(reply, "shutting_down"));
    free(reply);
    fl_mcp_destroy(mcp);

    uint8_t pixels[8 * 4 * 4];
    for (size_t i = 0; i < sizeof(pixels); ++i) pixels[i] = (uint8_t)(i * 7);
    size_t size = 0; uint32_t w = 0, h = 0;
    unsigned char *png = fl_png_encode_region(pixels, 8, 4, 2, 0, 6, 4, 3, &size, &w, &h);
    CHECK(png && size > 8 && !memcmp(png, "\x89PNG", 4) && w == 3 && h == 2);
    free(png);
    CHECK(fl_png_encode_region(pixels, 8, 4, 8, 0, 1, 1, 0, &size, NULL, NULL) == NULL);
}

/* History keeps invariants on its own: budgets drop the oldest entries, redo is cleared by new work. */
static void test_history(const char *directory)
{
    char dir[FL_PATH_MAX];
    CHECK(fl_fs_join(dir, sizeof(dir), directory, "history") && fl_fs_mkdir_p(dir));
    char manifest[FL_PATH_MAX];
    CHECK(fl_fs_join(manifest, sizeof(manifest), dir, "project.flow.json")); fl_fs_remove(manifest);
    Fl_Project *p = fl_project_create(dir, "History", NULL);
    CHECK(p != NULL); if (!p) return;
    Fl_History *h = fl_history_create(2, 1u << 30);
    CHECK(h != NULL);
    Fl_Id scene = fl_project_scene_at(p, 0)->id;
    const char *names[] = {"one", "two", "three"};
    for (int i = 0; i < 3; ++i) {
        CHECK(fl_history_begin(h, p, names[i]));
        CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_RENAME, .scene = scene, .name = names[i]}, NULL));
        CHECK(fl_history_commit(h, p));
    }
    CHECK(fl_history_undo_count(h) == 2);
    char label[FL_HISTORY_LABEL_MAX]; bool conflict;
    CHECK(fl_history_undo(h, p, label, &conflict) && !strcmp(label, "three") && !strcmp(fl_project_scene(p, scene)->name, "two"));
    CHECK(fl_history_undo(h, p, label, &conflict) && !strcmp(label, "two") && !strcmp(fl_project_scene(p, scene)->name, "one"));
    CHECK(!fl_history_undo(h, p, label, &conflict));
    CHECK(fl_history_redo(h, p, label, &conflict) && !strcmp(fl_project_scene(p, scene)->name, "two"));
    CHECK(fl_history_begin(h, p, "four"));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_RENAME, .scene = scene, .name = "four"}, NULL));
    CHECK(fl_history_commit(h, p));
    CHECK(fl_history_redo_count(h) == 0 && fl_history_undo_count(h) == 2);
    CHECK(fl_history_begin(h, p, "rolled back"));
    CHECK(fl_project_apply(p, &(Fl_Op){.kind = FL_OP_SCENE_RENAME, .scene = scene, .name = "doomed"}, NULL));
    CHECK(fl_history_rollback(h, p) && !strcmp(fl_project_scene(p, scene)->name, "four"));
    fl_history_destroy(h);
    fl_project_destroy(p);
}

/* Project assets need no scenes; JSON IDs must survive the full unsigned range without rounding. */
/* Prototype assets use the generic source protocol, exact aliases, inspection and shared undo. */
static void test_asset_sources(Fl_Agent *agent)
{
    unsigned char bytes[56]; fl_transform_encode(&(Fl_Transform){.width=100,.height=50,.scale_x=1,.scale_y=1},bytes);
    Fl_Component transform={.type=FL_COMPONENT_TRANSFORM,.version=1,.id=1,.size=sizeof(bytes),.data=bytes};
    Fl_Entity root={.id=1,.name="Template",.visible=true,.components=&transform,.component_count=1};
    Fl_Prototype prototype={.root=1,.next_id=2,.next_component_id=2,.scope={.duration=120000,.entities=&root,.entity_count=1}};
    Fl_Json context={0}; cJSON *document=fl_prototype_write(&context,&prototype);
    char *source=document ? cJSON_PrintUnformatted(document) : NULL; cJSON_Delete(document); CHECK(source);
    if (!source) return;
    cJSON *request=cJSON_CreateObject(), *ops=cJSON_AddArrayToObject(request,"ops"), *op=cJSON_CreateObject();
    cJSON_AddItemToArray(ops,op); cJSON_AddStringToObject(op,"op","asset.source"); cJSON_AddStringToObject(op,"kind","flproto");
    cJSON_AddStringToObject(op,"name","Reusable"); cJSON_AddStringToObject(op,"source",source); cJSON_AddStringToObject(op,"as","$proto");
    op=cJSON_CreateObject(); cJSON_AddItemToArray(ops,op); cJSON_AddStringToObject(op,"op","asset.rename");
    cJSON_AddStringToObject(op,"asset","$proto"); cJSON_AddStringToObject(op,"name","Reusable title");
    char *text=cJSON_PrintUnformatted(request); cJSON_Delete(request); cJSON *error=NULL;
    cJSON *result=call(agent,"apply_ops",text,&error); free(text); CHECK(result && !error);
    const Fl_Asset *asset=fl_project_asset_named(g_project,FL_ASSET_PROTOTYPE,"Reusable title"); CHECK(asset);
    Fl_Id id=asset ? asset->id : 0; cJSON_Delete(result);
    char query[128]; snprintf(query,sizeof(query),"{\"asset\":\"%llu\"}",(unsigned long long)id);
    result=call(agent,"get_asset",query,&error); CHECK(result && !error);
    const cJSON *value=result ? cJSON_GetObjectItemCaseSensitive(result,"asset") : NULL;
    CHECK(value && !strcmp(cJSON_GetObjectItemCaseSensitive(value,"source")->valuestring,source) && cJSON_GetObjectItemCaseSensitive(value,"path"));
    cJSON_Delete(result);
    result=call(agent,"apply_ops","{\"ops\":[{\"op\":\"asset.source\",\"kind\":\"flproto\",\"name\":\"Malformed\",\"source\":\"{}\"}]}",&error);
    CHECK(!result && error && error_code(error,"rejected")); cJSON_Delete(error); error=NULL;
    CHECK(fl_project_asset(g_project,id));
    char removal[160]; snprintf(removal,sizeof(removal),"{\"ops\":[{\"op\":\"asset.remove\",\"asset\":\"%llu\"}]}",(unsigned long long)id);
    result=call(agent,"apply_ops",removal,&error); CHECK(result && !error && !fl_project_asset(g_project,id)); cJSON_Delete(result);
    result=call(agent,"undo","{}",&error); CHECK(result && !error && fl_project_asset(g_project,id)); cJSON_Delete(result);
    free(source);
}

static void test_project_assets_and_exact_ids(Fl_Agent *agent, const char *root)
{
    char directory[FL_PATH_MAX], manifest[FL_PATH_MAX];
    CHECK(fl_fs_join(directory,sizeof(directory),root,"asset_ids") && fl_fs_mkdir_p(directory));
    Fl_Project *previous = g_project;
    g_project = fl_project_create(directory,"Exact IDs",NULL);
    CHECK(g_project); if (!g_project) {g_project=previous; return;}
    Fl_Id original = fl_project_scene_at(g_project,0)->id;
    CHECK(fl_project_apply(g_project,&(Fl_Op){.kind=FL_OP_SCENE_REMOVE,.scene=original},NULL));
    cJSON *error=NULL;
    cJSON *result=call(agent,"set_script","{\"script\":\"Unattached\",\"source\":\"return {sample=function(ctx) end}\"}",&error);
    CHECK(result && !error); cJSON_Delete(result);
    result=call(agent,"get_script","{\"script\":\"Unattached\"}",&error);
    const cJSON *asset=result ? cJSON_GetObjectItemCaseSensitive(result,"script") : NULL;
    CHECK(asset && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(asset,"used_by"))==0);
    cJSON_Delete(result);
    CHECK(fl_project_save(g_project)); fl_project_destroy(g_project); g_project=NULL;
    CHECK(fl_fs_join(manifest,sizeof(manifest),directory,"project.flow.json"));
    size_t size=0; char *text=fl_fs_read_file(manifest,1024*1024,&size);
    cJSON *document=text ? cJSON_Parse(text) : NULL; free(text);
    CHECK(document && cJSON_ReplaceItemInObjectCaseSensitive(document,"next_id",cJSON_CreateString("9007199254740993")));
    text=document ? cJSON_Print(document) : NULL;
    CHECK(text && fl_fs_write_atomic(manifest,text,strlen(text))); cJSON_free(text); cJSON_Delete(document);
    g_project=fl_project_open(directory,NULL); CHECK(g_project);
    if (g_project) {
        result=call(agent,"apply_ops","{\"ops\":[{\"op\":\"scene.add\",\"name\":\"Large\",\"as\":\"$scene\"},{\"op\":\"entity.create\",\"scene\":\"$scene\",\"kind\":\"rectangle\",\"as\":\"$object\"}]}",&error);
        CHECK(result && !error);
        const cJSON *list=result ? cJSON_GetObjectItemCaseSensitive(result,"results") : NULL;
        const cJSON *id=list && list->child ? cJSON_GetObjectItemCaseSensitive(list->child,"id") : NULL;
        CHECK(cJSON_IsString(id) && !strcmp(id->valuestring,"9007199254740993")); cJSON_Delete(result);
        result=call(agent,"get_scene","{\"scene\":\"9007199254740993\"}",&error);
        CHECK(result && !error); cJSON_Delete(result);
        result=call(agent,"get_scene","{\"scene\":\"18446744073709551616\"}",&error);
        CHECK(!result && error && error_code(error,"invalid_argument")); cJSON_Delete(error); error=NULL;
        result=call(agent,"get_scene","{\"scene\":\"-1\"}",&error);
        CHECK(!result && error && error_code(error,"invalid_argument")); cJSON_Delete(error);
        fl_project_destroy(g_project);
    }
    g_project=previous;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char directory[FL_PATH_MAX], manifest[FL_PATH_MAX];
    CHECK(fl_fs_join(directory, sizeof(directory), argv[1], "agent") && fl_fs_mkdir_p(directory));
    CHECK(fl_fs_join(manifest, sizeof(manifest), directory, "project.flow.json")); fl_fs_remove(manifest);
    g_project = fl_project_create(directory, "Agent", NULL);
    CHECK(g_project != NULL); if (!g_project) return 1;
    Fl_Agent *agent = fl_agent_create(&(Fl_AgentHost){host_project, fake_render, NULL});
    CHECK(agent != NULL); if (!agent) return 1;
    Fl_Id scene = fl_project_scene_at(g_project, 0)->id;
    test_authoring(agent, scene);
    test_look(agent, scene);
    test_scripts(agent, scene);
    test_asset_sources(agent);
    test_scene_history(agent, directory);
    test_protocol(agent);
    Fl_Agent *http_agent = fl_agent_create(&(Fl_AgentHost){host_project, fake_render, NULL});
    CHECK(http_agent != NULL);
    test_http(http_agent);
    fl_agent_destroy(http_agent);
    test_history(argv[1]);
    test_timeout_and_png();
    test_project_assets_and_exact_ids(agent,argv[1]);
    fl_agent_destroy(agent);
    fl_project_destroy(g_project);

    return FLOW_TEST_RESULT("fl_agent_tests");
}

#pragma once
#include "fl_agent.h"
#include "project/fl_bake.h"
#include "project/fl_history.h"
#include "schema/fl_widgets.h"
#include "time/fl_scene_state.h"
#include "render/fl_frame.h"

#define FL_AGENT_MAX_OPS 2000u
#define FL_AGENT_MAX_REFS 512u

#define FL_AGENT_MAX_FRAMES 6u

/* PNG frames produced by a tool, handed to the transport after the call returns. */
typedef struct Fl_AgentFrames {
    unsigned char *png[FL_AGENT_MAX_FRAMES];
    size_t size[FL_AGENT_MAX_FRAMES];
    size_t count;
} Fl_AgentFrames;

struct Fl_Agent {
    Fl_AgentHost host;
    Fl_Bake *bake;
    uint64_t bake_token;
    Fl_AgentFrames frames;
    uint64_t sheet_hash;
    Fl_Id sheet_scene;
    char review[4096];
    Fl_Id touched[64];
    size_t touched_count;
    void *bindings;             /* tool registrations handed to the MCP core */
};

/* Names given to created objects by `as`, resolved from "$name" arguments in later operations. */
typedef struct Fl_AgentRefs {
    char (*names)[48];
    Fl_Id *ids;
    size_t count;
} Fl_AgentRefs;

/* Argument parsing. Every function returns false after recording a failure. `ctx` prefixes messages. */
bool agent_only_keys(const cJSON *object, Fl_Fail *fail, const char *ctx, const char *const *allowed);
bool agent_object(const cJSON *value, Fl_Fail *fail, const char *ctx);
bool agent_get_string(const cJSON *object, const char *key, bool required, const char **out, Fl_Fail *fail, const char *ctx);
bool agent_get_number(const cJSON *object, const char *key, bool required, double *out, bool *present, Fl_Fail *fail, const char *ctx);
bool agent_get_bool(const cJSON *object, const char *key, bool *out, bool *present, Fl_Fail *fail, const char *ctx);
/* Parse one exact ID or same-batch name; safe numeric IDs and unsigned decimal strings are accepted. */
bool agent_id_value(const cJSON *value, const Fl_AgentRefs *refs, Fl_Id *out, Fl_Fail *fail, const char *ctx);
/* Emit exact IDs as numbers within JSON's safe range, otherwise as decimal strings. */
cJSON *agent_id_json(Fl_Id id);
bool agent_get_id(const cJSON *object, const char *key, const Fl_AgentRefs *refs, bool required, Fl_Id *out, Fl_Fail *fail, const char *ctx);
bool agent_get_seconds(const cJSON *object, const char *key, bool required, Fl_Tick *out, bool *present, Fl_Fail *fail, const char *ctx);
bool agent_parse_color(const cJSON *value, unsigned char rgba[4], Fl_Fail *fail, const char *ctx);
bool agent_parse_anchor(const cJSON *value, const Fl_AgentRefs *refs, Fl_TimeAnchor *out, Fl_Fail *fail, const char *ctx);

/* Unit conversion. Seconds are rounded to the nearest tick and bounded so tick arithmetic cannot overflow. */
bool agent_seconds_to_ticks(double seconds, Fl_Tick *ticks);
double agent_ticks_to_seconds(int64_t ticks);

/* Names for the closed vocabularies exchanged with agents. */
const char *agent_widget_name(Fl_WidgetKind kind);
bool agent_widget_from_name(const char *name, Fl_WidgetKind *kind);
bool agent_property_from_name(const char *name, Fl_AnimProperty *property);
const char *agent_ease_name(Fl_Ease ease);
bool agent_ease_from_name(const char *name, Fl_Ease *ease);

/* Locate an entity/beat/clip/key/track anywhere in the project. */
const Fl_Scene *agent_scene_of_entity(const Fl_Project *project, Fl_Id entity);
const Fl_Scene *agent_scene_of_beat(const Fl_Project *project, Fl_Id beat);
const Fl_Scene *agent_scene_of_clip(const Fl_Project *project, Fl_Id clip);
/* One project Lua asset: id, name, bytes, the entities running it and, with source, its text. NULL on allocation failure. */
cJSON *agent_script_json(const Fl_Project *project, const Fl_Asset *script, bool with_source);
const Fl_Scene *agent_scene_of_key(const Fl_Project *project, Fl_Id key, const Fl_Track **track);

/* JSON views of the document. */
cJSON *agent_scene_summary(const Fl_Scene *scene);
cJSON *agent_scene_json(const Fl_Project *project, const Fl_Scene *scene, bool with_sources);
cJSON *agent_asset_json(const Fl_Asset *asset, size_t uses);
cJSON *agent_anchor_json(const Fl_Scene *scene, const Fl_TimeAnchor *anchor);
void agent_color_json(cJSON *object, const char *key, const unsigned char rgba[4]);

/* Apply a batch of operations atomically. */
cJSON *agent_tool_apply_ops(Fl_Agent *agent, const cJSON *args, cJSON **error);

/* Static guidance text. */
extern const char *const FL_AGENT_REFERENCE[];   /* NULL-terminated sections */

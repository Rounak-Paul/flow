#include "project/fl_prototype_authoring.h"
#include "project/fl_scope_authoring.h"
#include "fl_agent_internal.h"
#include "doc/fl_document.h"
#include "components/fl_script.h"
#include "scripting/fl_script_runtime.h"
#include "components/fl_spatial.h"
#include "components/fl_style.h"
#include "components/fl_motion.h"
#include "components/fl_path.h"
#include "components/fl_effect.h"
#include "schema/fl_properties.h"
#include "components/fl_video_clip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEG_TO_RAD (3.14159265358979323846 / 180.0)

/* Execution state of one batch: the project and the names created so far. */
typedef struct Ctx {
    Fl_Project *project;
    Fl_AgentRefs refs;
} Ctx;

typedef bool (*OpFn)(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *fail);

/* Add the created id to the result so later operations and the caller can use it. */
static void report_id(cJSON *result, Fl_Id id) { cJSON_AddItemToObject(result, "id", agent_id_json(id)); }

static const Fl_Scene *need_scene(Ctx *x, const cJSON *op, const char *key, Fl_Fail *f, const char *ctx)
{
    Fl_Id id;
    if (!agent_get_id(op, key, &x->refs, true, &id, f, ctx)) return NULL;
    const Fl_Scene *s = fl_project_scene(x->project, id);
    if (!s) fl_fail(f, "not_found", "%s: no scene with id %llu", ctx, (unsigned long long)id);
    return s;
}

static const Fl_Entity *need_entity(Ctx *x, const cJSON *op, const char *key, const Fl_Scene **scene, Fl_Fail *f, const char *ctx)
{
    Fl_Id id;
    if (!agent_get_id(op, key, &x->refs, true, &id, f, ctx)) return NULL;
    const Fl_Scene *s = agent_scene_of_entity(x->project, id);
    if (!s) {fl_fail(f, "not_found", "%s: no entity with id %llu", ctx, (unsigned long long)id); return NULL;}
    if (scene) *scene = s;
    return fl_scene_entity(s, id);
}

/* Document-level rejection: the operation parsed but fl_project_apply refused it. */
static bool rejected(Fl_Fail *f, const char *ctx, const char *hint)
{
    return fl_fail(f, "rejected", "%s was rejected by document validation: %s", ctx, hint);
}

static bool apply(Ctx *x, const Fl_Op *op, Fl_Id *created) { return fl_project_apply(x->project, op, created); }

/* ---------- scenes ---------- */

static bool scene_add(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "name", NULL};
    const char *name;
    if (!agent_only_keys(op, f, "scene.add", keys) || !agent_get_string(op, "name", true, &name, f, "scene.add")) return false;
    Fl_Id id = 0;
    if (!name[0] || !apply(x, &(Fl_Op){.kind = FL_OP_SCENE_ADD, .name = name}, &id)) return rejected(f, "scene.add", "name must be non-empty");
    report_id(result, id); return true;
}

static bool scene_remove(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "scene", NULL};
    (void)result;
    const Fl_Scene *s;
    if (!agent_only_keys(op, f, "scene.remove", keys) || !(s = need_scene(x, op, "scene", f, "scene.remove"))) return false;
    return apply(x, &(Fl_Op){.kind = FL_OP_SCENE_REMOVE, .scene = s->id}, NULL) || rejected(f, "scene.remove", "scene");
}

static bool scene_rename(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "scene", "name", NULL};
    (void)result;
    const Fl_Scene *s; const char *name;
    if (!agent_only_keys(op, f, "scene.rename", keys) || !(s = need_scene(x, op, "scene", f, "scene.rename")) ||
        !agent_get_string(op, "name", true, &name, f, "scene.rename")) return false;
    return apply(x, &(Fl_Op){.kind = FL_OP_SCENE_RENAME, .scene = s->id, .name = name}, NULL) || rejected(f, "scene.rename", "name must be non-empty");
}

static bool scene_move(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "scene", "position", NULL};
    (void)result;
    const Fl_Scene *s; double position;
    if (!agent_only_keys(op, f, "scene.move", keys) || !(s = need_scene(x, op, "scene", f, "scene.move")) ||
        !agent_get_number(op, "position", true, &position, NULL, f, "scene.move")) return false;
    return (position >= 0 && position < (double)fl_project_scene_count(x->project) &&
            apply(x, &(Fl_Op){.kind = FL_OP_SCENE_MOVE, .scene = s->id, .position = (size_t)position}, NULL)) ||
           rejected(f, "scene.move", "position must be an index inside the scene list");
}

/* Settings that change canvas size, framing and camera; omitted fields keep their value. */
static bool scene_set(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "scene", "width", "height", "aspect", "camera", "background", NULL};
    (void)result;
    const Fl_Scene *s;
    if (!agent_only_keys(op, f, "scene.set", keys) || !(s = need_scene(x, op, "scene", f, "scene.set"))) return false;
    double width = (double)s->width, height = (double)s->height;
    bool has_width, has_height;
    if (!agent_get_number(op, "width", false, &width, &has_width, f, "scene.set") ||
        !agent_get_number(op, "height", false, &height, &has_height, f, "scene.set")) return false;
    if (has_width || has_height) {
        if (width < 1 || height < 1 || width > 65536 || height > 65536 || width != floor(width) || height != floor(height))
            return fl_fail(f, "invalid_argument", "scene.set: width and height are whole pixels in 1..65536");
        if (!apply(x, &(Fl_Op){.kind = FL_OP_SCENE_SETTINGS, .scene = s->id, .width = (uint64_t)width, .height = (uint64_t)height,
                               .duration = (uint64_t)fl_scene_length(s)}, NULL)) return rejected(f, "scene.set", "canvas size");
    }
    const cJSON *aspect = cJSON_GetObjectItemCaseSensitive(op, "aspect");
    if (aspect) {
        uint64_t w = 0, h = 0;
        if (!cJSON_IsNull(aspect)) {
            const cJSON *a = cJSON_IsArray(aspect) && cJSON_GetArraySize(aspect) == 2 ? aspect->child : NULL;
            if (!a || !cJSON_IsNumber(a) || !cJSON_IsNumber(a->next) || a->valuedouble < 1 || a->next->valuedouble < 1 ||
                a->valuedouble > 65536 || a->next->valuedouble > 65536) return fl_fail(f, "invalid_argument", "scene.set: aspect is [width, height] or null");
            w = (uint64_t)a->valuedouble; h = (uint64_t)a->next->valuedouble;
        }
        if (!apply(x, &(Fl_Op){.kind = FL_OP_SCENE_ASPECT, .scene = s->id, .aspect_width = w, .aspect_height = h}, NULL))
            return rejected(f, "scene.set", "aspect");
    }
    const cJSON *background = cJSON_GetObjectItemCaseSensitive(op, "background");
    if (background) {
        unsigned char rgba[4];
        if (!agent_parse_color(background, rgba, f, "scene.set background")) return false;
        if (rgba[3] != 255) return fl_fail(f, "invalid_argument", "scene.set: the background is opaque; leave out the alpha channel");
        if (!apply(x, &(Fl_Op){.kind = FL_OP_SCENE_BACKGROUND, .scene = s->id, .background = {rgba[0], rgba[1], rgba[2], 255}}, NULL))
            return rejected(f, "scene.set", "background");
    }
    const cJSON *camera = cJSON_GetObjectItemCaseSensitive(op, "camera");
    if (camera) {
        const cJSON *c = cJSON_IsArray(camera) && cJSON_GetArraySize(camera) == 2 ? camera->child : NULL;
        if (!c || !cJSON_IsNumber(c) || !cJSON_IsNumber(c->next) || !isfinite(c->valuedouble) || !isfinite(c->next->valuedouble))
            return fl_fail(f, "invalid_argument", "scene.set: camera is [x, y]");
        if (!apply(x, &(Fl_Op){.kind = FL_OP_SCENE_CAMERA, .scene = s->id, .camera_x = c->valuedouble, .camera_y = c->next->valuedouble}, NULL))
            return rejected(f, "scene.set", "camera");
    }
    return true;
}

/* ---------- entities ---------- */

/* Longest value of a text content or typography family accepted from an agent. */
#define TEXT_LIMIT (1u << 20)

/* Merge `props` into the entity's current components and commit them in one configure operation. */
static bool configure(Ctx *x, const Fl_Scene *scene, const Fl_Entity *e, const cJSON *props, Fl_Fail *f, const char *ctx)
{
    static const char *const keys[] = {"name", "visible", "x", "y", "z", "width", "height", "depth", "rotation", "rx", "ry", "scale_x", "scale_y", "scale_z", "opacity",
        "fill", "stroke", "stroke_width", "text", "font_size", "font_family", "font_weight", "italic", "asset",
        "radius", "fill2", "gradient_angle", "shadow_blur", "shadow_x", "shadow_y", "shadow_color", "align",
        "start", "offset", "rate", "loop", "crop", "clip", "blur", "bloom",
        "pivot_x", "pivot_y", "pivot_z", "glow", "blend", "radial", "gradient_mid", "gradient_mid_pos",
        "tracking", "stagger", "rise", "text_fade", "text_duration", "trim_start", "trim_end", "commands",
        "vignette", "grain", "lift", "gamma", "gain",
        "life", "size", "speed", "size_end", "gravity", "spread", "burst", "particle_blend", "force_x", "force_y", "force_z", "color_end",
        "effect_kind", "u0", "u1", "u2", "u3", "u4", "u5", "u6", "u7", "source", NULL};
    if (!agent_object(props, f, ctx) || !agent_only_keys(props, f, ctx, keys)) return false;
    const Fl_WidgetDef *def = fl_widget_for_entity(e);
    Fl_Pose t = fl_pose_identity(); Fl_Bounds box = fl_entity_bounds(e);
    fl_pose_decode(fl_entity_component(e, FL_COMPONENT_POSE), &t);
    Fl_Appearance a = fl_widget_appearance(e);
    Fl_Typography ty = fl_widget_typography(e);
    double opacity = fl_widget_opacity(e), number;
    Fl_Id asset = 0;
    fl_image_asset_decode(fl_entity_component(e, FL_COMPONENT_IMAGE), &asset);
    Fl_VideoClip clip = {.rate = 1};
    bool has_clip = fl_video_clip_decode(fl_entity_component(e, FL_COMPONENT_VIDEO), &clip);
    Fl_Style style = fl_entity_style(e);
    bool style_given = false;
    const Fl_Component *current_text = fl_entity_component(e, FL_COMPONENT_TEXT);
    const char *name = e->name, *text = NULL;
    bool visible = e->visible, present;
    if (!agent_get_string(props, "name", false, &name, f, ctx)) return false;
    if (!name) name = e->name;
    if (!name[0]) return fl_fail(f, "invalid_argument", "%s: name must be non-empty", ctx);
    if (!agent_get_bool(props, "visible", &visible, NULL, f, ctx)) return false;
    double *pose_fields[] = {&t.x, &t.y, &t.z, &t.rx, &t.ry, &t.rz, &t.sx, &t.sy, &t.sz};
    static const char *const pose_keys[] = {"x", "y", "z", "rx", "ry", "rotation", "scale_x", "scale_y", "scale_z"};
    static const int pose_degrees[] = {0, 0, 0, 1, 1, 1, 0, 0, 0};
    for (size_t i = 0; i < 9; ++i) {
        if (!agent_get_number(props, pose_keys[i], false, &number, &present, f, ctx)) return false;
        if (!present) continue;
        if (fabs(number) > 1e9) return fl_fail(f, "invalid_argument", "%s: \"%s\" is out of range", ctx, pose_keys[i]);
        *pose_fields[i] = pose_degrees[i] ? number * DEG_TO_RAD : number;
    }
    double *pivots[] = {&t.px, &t.py, &t.pz};
    static const char *const pivot_keys[] = {"pivot_x", "pivot_y", "pivot_z"};
    for (size_t i = 0; i < 3; ++i) {
        if (!agent_get_number(props, pivot_keys[i], false, &number, &present, f, ctx)) return false;
        if (present) *pivots[i] = number;
    }
    double *box_fields[] = {&box.width, &box.height, &box.depth};
    static const char *const box_keys[] = {"width", "height", "depth"};
    for (size_t i = 0; i < 3; ++i) {
        if (!agent_get_number(props, box_keys[i], false, &number, &present, f, ctx)) return false;
        if (!present) continue;
        if (number < 0 || number > 1e9) return fl_fail(f, "invalid_argument", "%s: \"%s\" is out of range", ctx, box_keys[i]);
        *box_fields[i] = number;
    }
    if (!agent_get_number(props, "opacity", false, &number, &present, f, ctx)) return false;
    if (present) {
        if (number < 0 || number > 1) return fl_fail(f, "invalid_argument", "%s: opacity is between 0 and 1", ctx);
        opacity = number;
    }
    const cJSON *item;
    if ((item = cJSON_GetObjectItemCaseSensitive(props, "fill")) && !cJSON_IsNull(item)) {
        if (!def || !def->has_fill) return fl_fail(f, "not_applicable", "%s: a %s has no fill", ctx, def ? agent_widget_name(def->kind) : "entity");
        if (!agent_parse_color(item, a.fill, f, "fill")) return false;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(props, "stroke")) && !cJSON_IsNull(item)) {
        if (!def || !def->has_stroke) return fl_fail(f, "not_applicable", "%s: a %s has no stroke", ctx, def ? agent_widget_name(def->kind) : "entity");
        if (!agent_parse_color(item, a.stroke, f, "stroke")) return false;
    }
    if (!agent_get_number(props, "stroke_width", false, &number, &present, f, ctx)) return false;
    if (present) {
        if (!def || !def->has_stroke) return fl_fail(f, "not_applicable", "%s: stroke_width needs a shape with a stroke", ctx);
        if (number < 0 || number > 65536) return fl_fail(f, "invalid_argument", "%s: stroke_width is 0..65536", ctx);
        a.stroke_width = number;
    }
    if (!agent_get_string(props, "text", false, &text, f, ctx)) return false;
    if (text && (!def || !def->has_text)) return fl_fail(f, "not_applicable", "%s: only text entities have text", ctx);
    if (text && strlen(text) > TEXT_LIMIT) return fl_fail(f, "invalid_argument", "%s: text is longer than %u bytes", ctx, TEXT_LIMIT);
    if (!agent_get_number(props, "font_size", false, &number, &present, f, ctx)) return false;
    if (present) {
        if (!def || !def->has_text) return fl_fail(f, "not_applicable", "%s: font_size needs a text entity", ctx);
        if (number < 1 || number > 2048) return fl_fail(f, "invalid_argument", "%s: font_size is 1..2048", ctx);
        a.font_size = number;
    }
    const char *family = NULL;
    if (!agent_get_string(props, "font_family", false, &family, f, ctx)) return false;
    if (family) {
        if (!def || !def->has_text) return fl_fail(f, "not_applicable", "%s: font_family needs a text entity", ctx);
        if (!family[0] || strlen(family) > FL_TYPOGRAPHY_FAMILY_MAX) return fl_fail(f, "invalid_argument", "%s: font_family must be 1..%d characters", ctx, FL_TYPOGRAPHY_FAMILY_MAX);
        snprintf(ty.family, sizeof(ty.family), "%s", family);
    }
    if (!agent_get_number(props, "font_weight", false, &number, &present, f, ctx)) return false;
    if (present) {
        if (!def || !def->has_text) return fl_fail(f, "not_applicable", "%s: font_weight needs a text entity", ctx);
        if (number < 1 || number > 1000 || number != floor(number)) return fl_fail(f, "invalid_argument", "%s: font_weight is a whole number 1..1000", ctx);
        ty.weight = (unsigned)number;
    }
    bool italic = ty.italic;
    if (!agent_get_bool(props, "italic", &italic, &present, f, ctx)) return false;
    if (present && (!def || !def->has_text)) return fl_fail(f, "not_applicable", "%s: italic needs a text entity", ctx);
    ty.italic = italic;
    if (!agent_get_number(props, "asset", false, &number, &present, f, ctx)) return false;
    if (present) {
        if (!def || !(def->has_image || def->has_video)) return fl_fail(f, "not_applicable", "%s: only image and video entities show an asset", ctx);
        const Fl_Asset *record = fl_project_asset(x->project, (Fl_Id)number);
        Fl_AssetKind wanted = def->has_video ? FL_ASSET_VIDEO : FL_ASSET_IMAGE;
        if (number < 1 || !record || record->kind != wanted) return fl_fail(f, "not_found", "%s: asset %.0f is not %s in this project", ctx, number, def->has_video ? "a video" : "an image");
        asset = (Fl_Id)number; clip.asset = asset; has_clip = true;
    }
    if (def && def->has_video) {
        if (!agent_get_seconds(props, "start", false, &clip.start, &present, f, ctx)) return false;
        if (!agent_get_seconds(props, "offset", false, &clip.offset, &present, f, ctx)) return false;
        if (clip.offset < 0) return fl_fail(f, "invalid_argument", "%s: offset cannot be negative", ctx);
        if (!agent_get_number(props, "rate", false, &number, &present, f, ctx)) return false;
        if (present) {
            if (!(number > 0 && number <= 16)) return fl_fail(f, "invalid_argument", "%s: rate is greater than 0 and at most 16", ctx);
            clip.rate = number;
        }
        if (!agent_get_bool(props, "loop", &clip.loop, NULL, f, ctx)) return false;
    } else {
        bool particles = fl_entity_component(e, FL_COMPONENT_PARTICLES) != NULL;
        for (const char *key = "start"; key; key = !strcmp(key, "start") ? "offset" : !strcmp(key, "offset") ? "rate" : !strcmp(key, "rate") ? "loop" : NULL)
            if (cJSON_GetObjectItemCaseSensitive(props, key) && !(particles && !strcmp(key, "rate"))) return fl_fail(f, "not_applicable", "%s: %s needs a video entity", ctx, key);
    }
    const char *shape_kinds = "rectangle, image or video";
    if (!agent_get_number(props, "radius", false, &number, &present, f, ctx)) return false;
    if (present) {
        if (!def || !(def->kind == FL_WIDGET_RECTANGLE || def->kind == FL_WIDGET_ENTITY || def->has_image || def->has_video)) return fl_fail(f, "not_applicable", "%s: radius needs a %s (or an entity that clips)", ctx, shape_kinds);
        if (number < 0 || number > 65536) return fl_fail(f, "invalid_argument", "%s: radius is 0..65536", ctx);
        style.radius = number; style_given = true;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(props, "fill2"))) {
        if (!def || !(def->kind == FL_WIDGET_RECTANGLE || def->kind == FL_WIDGET_ELLIPSE || fl_widget_is_polygon(def->kind))) return fl_fail(f, "not_applicable", "%s: gradients need a rectangle, ellipse or polygon shape", ctx);
        style.gradient = !cJSON_IsNull(item); style_given = true;
        if (style.gradient && !agent_parse_color(item, style.gradient_to, f, "fill2")) return false;
    }
    if (!agent_get_number(props, "gradient_angle", false, &number, &present, f, ctx)) return false;
    if (present) {
        if (!def || !(def->kind == FL_WIDGET_RECTANGLE || def->kind == FL_WIDGET_ELLIPSE)) return fl_fail(f, "not_applicable", "%s: gradients need a rectangle or ellipse", ctx);
        style.gradient_angle = number * DEG_TO_RAD; style_given = true;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(props, "crop")) && !cJSON_IsNull(item)) {
        if (!def || !(def->has_image || def->has_video)) return fl_fail(f, "not_applicable", "%s: crop needs an image or video", ctx);
        const cJSON *c = cJSON_IsArray(item) && cJSON_GetArraySize(item) == 4 ? item->child : NULL;
        double region[4];
        for (int i = 0; i < 4; ++i, c = c ? c->next : NULL) {
            if (!c || !cJSON_IsNumber(c) || !isfinite(c->valuedouble)) return fl_fail(f, "invalid_argument", "%s: crop is [x, y, width, height] as fractions of the source", ctx);
            region[i] = c->valuedouble;
        }
        if (region[0] < 0 || region[1] < 0 || region[2] < 0.001 || region[3] < 0.001 || region[0] + region[2] > 1 + 1e-9 || region[1] + region[3] > 1 + 1e-9)
            return fl_fail(f, "invalid_argument", "%s: crop must stay inside the source: x, y >= 0, width, height >= 0.001, x + width <= 1, y + height <= 1", ctx);
        memcpy(style.crop, region, sizeof(region)); style_given = true;
    }
    if (!agent_get_bool(props, "clip", &style.clip, &present, f, ctx)) return false;
    if (present) style_given = true;
    if (!agent_get_number(props, "blur", false, &number, &present, f, ctx)) return false;
    if (present) {
        if (number < 0 || number > 512) return fl_fail(f, "invalid_argument", "%s: blur is 0..512 scene pixels", ctx);
        style.blur = number; style_given = true;
    }
    static const char *const look_keys[] = {"glow", "blend", "radial", "gradient_mid_pos"};
    double *look_fields[] = {&style.glow, NULL, NULL, &style.gradient_mid_pos};
    for (size_t i = 0; i < 4; ++i) {
        if (!agent_get_number(props, look_keys[i], false, &number, &present, f, ctx)) return false;
        if (!present) continue;
        if (i == 1) { if (number < 0 || number > 3) return fl_fail(f, "invalid_argument", "%s: blend is 0 normal, 1 add, 2 multiply, 3 screen", ctx); style.blend = (unsigned char)number; }
        else if (i == 2) style.radial = number >= 0.5;
        else *look_fields[i] = number;
        style_given = true;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(props, "gradient_mid")) && !cJSON_IsNull(item)) {
        if (!agent_parse_color(item, style.gradient_mid, f, "gradient_mid")) return false;
        style_given = true;
    }
    bool shadowable = def && (def->kind == FL_WIDGET_RECTANGLE || def->kind == FL_WIDGET_ELLIPSE || def->has_image || def->has_video);
    static const char *const shadow_keys[] = {"shadow_blur", "shadow_x", "shadow_y"};
    double *shadow_fields[] = {&style.shadow_blur, &style.shadow_x, &style.shadow_y};
    for (size_t i = 0; i < 3; ++i) {
        if (!agent_get_number(props, shadow_keys[i], false, &number, &present, f, ctx)) return false;
        if (!present) continue;
        if (!shadowable) return fl_fail(f, "not_applicable", "%s: %s needs a rectangle, ellipse, image or video", ctx, shadow_keys[i]);
        if (fabs(number) > 65536 || (i == 0 && (number < 0 || number > 4096))) return fl_fail(f, "invalid_argument", "%s: %s is out of range", ctx, shadow_keys[i]);
        *shadow_fields[i] = number; style_given = true;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(props, "shadow_color")) && !cJSON_IsNull(item)) {
        if (!shadowable) return fl_fail(f, "not_applicable", "%s: shadow_color needs a rectangle, ellipse, image or video", ctx);
        if (!agent_parse_color(item, style.shadow_color, f, "shadow_color")) return false;
        style_given = true;
    }
    const char *align = NULL;
    if (!agent_get_string(props, "align", false, &align, f, ctx)) return false;
    if (align) {
        if (!def || !def->has_text) return fl_fail(f, "not_applicable", "%s: align needs a text entity", ctx);
        if (!strcmp(align, "left")) style.align = FL_ALIGN_LEFT; else if (!strcmp(align, "center")) style.align = FL_ALIGN_CENTER;
        else if (!strcmp(align, "right")) style.align = FL_ALIGN_RIGHT; else return fl_fail(f, "invalid_argument", "%s: align is left, center or right", ctx);
        style_given = true;
    }
    if (style.shadow_blur > 0 && !style.shadow_color[3] && style_given) {
        static const unsigned char black[4] = {0, 0, 0, 160};
        if (!cJSON_GetObjectItemCaseSensitive(props, "shadow_color")) memcpy(style.shadow_color, black, 4);
    }

    unsigned char pose_bytes[FL_POSE_BYTES], bounds_bytes[FL_BOUNDS_BYTES], appearance[24], typography[3 + FL_TYPOGRAPHY_FAMILY_MAX], image[8], alpha[8], style_bytes[FL_STYLE_BYTES], clip_bytes[FL_VIDEO_CLIP_BYTES];
    unsigned char motion_bytes[FL_MOTION_BYTES], post_bytes[FL_POST_BYTES], particle_bytes[FL_PARTICLES_BYTES], effect_bytes[FL_EFFECT_HEADER + 65536];
    unsigned char path_bytes[24 + FL_CURVE_MAX * 49];
    fl_pose_encode(&t, pose_bytes); fl_bounds_encode(&box, bounds_bytes); fl_appearance_encode(&a, appearance); fl_opacity_encode(opacity, alpha);
    Fl_Component components[16] = {{.type=FL_COMPONENT_POSE, .version=1, .size=FL_POSE_BYTES, .data=pose_bytes}, {.type=FL_COMPONENT_BOUNDS, .version=1, .size=FL_BOUNDS_BYTES, .data=bounds_bytes}, {.type=FL_COMPONENT_OPACITY, .version=1, .size=8, .data=alpha}};
    size_t count = 3;
    if (def && def->kind != FL_WIDGET_ENTITY && !def->has_image && !def->has_video) components[count++] = (Fl_Component){.type = FL_COMPONENT_APPEARANCE, .version = 1, .size = 24, .data = appearance};
    if (def && def->has_text) {
        size_t size = fl_typography_encode(&ty, typography);
        if (!size) return fl_fail(f, "invalid_argument", "%s: invalid typography", ctx);
        const unsigned char *bytes = text ? (const unsigned char *)text : current_text ? current_text->data : (const unsigned char *)"";
        components[count++] = (Fl_Component){.type = FL_COMPONENT_TEXT, .version = 1, .size = text ? strlen(text) : current_text ? current_text->size : 0, .data = bytes};
        components[count++] = (Fl_Component){.type = FL_COMPONENT_TYPOGRAPHY, .version = 1, .size = size, .data = typography};
    }
    if (def && def->has_image && asset) {
        fl_image_asset_encode(asset, image);
        components[count++] = (Fl_Component){.type = FL_COMPONENT_IMAGE, .version = 1, .size = 8, .data = image};
    }
    if (def && def->has_video && has_clip) {
        fl_video_clip_encode(&clip, clip_bytes);
        components[count++] = (Fl_Component){.type = FL_COMPONENT_VIDEO, .version = 1, .size = FL_VIDEO_CLIP_BYTES, .data = clip_bytes};
    }
    if (style_given || fl_entity_component(e, FL_COMPONENT_STYLE)) {
        fl_style_encode(&style, style_bytes);
        components[count++] = (Fl_Component){.type = FL_COMPONENT_STYLE, .version = 1, .size = FL_STYLE_BYTES, .data = style_bytes};
    }
    if (def && def->has_text && (cJSON_GetObjectItemCaseSensitive(props, "tracking") || cJSON_GetObjectItemCaseSensitive(props, "stagger") || cJSON_GetObjectItemCaseSensitive(props, "rise") || cJSON_GetObjectItemCaseSensitive(props, "text_fade") || cJSON_GetObjectItemCaseSensitive(props, "text_duration") || fl_entity_component(e, FL_COMPONENT_TEXT_MOTION))) {
        Fl_TextMotion motion = fl_motion_default();
        fl_motion_decode(fl_entity_component(e, FL_COMPONENT_TEXT_MOTION), &motion);
        static const char *const motion_keys[] = {"tracking", "stagger", "rise", "text_fade", "text_duration"};
        double *motion_fields[] = {&motion.tracking, &motion.stagger, &motion.rise, &motion.fade, &motion.duration};
        for (size_t i = 0; i < 5; ++i) {
            if (!agent_get_number(props, motion_keys[i], false, &number, &present, f, ctx)) return false;
            if (present) *motion_fields[i] = number;
        }
        fl_motion_encode(&motion, motion_bytes);
        if (!fl_motion_decode(&(Fl_Component){.type=FL_COMPONENT_TEXT_MOTION,.version=1,.size=FL_MOTION_BYTES,.data=motion_bytes}, &motion)) return fl_fail(f, "invalid_argument", "%s: text motion is out of range", ctx);
        components[count++] = (Fl_Component){.type = FL_COMPONENT_TEXT_MOTION, .version = 1, .size = FL_MOTION_BYTES, .data = motion_bytes};
    }
    if (fl_entity_component(e, FL_COMPONENT_PATH) || cJSON_GetObjectItemCaseSensitive(props, "commands")) {
        Fl_Path path = fl_path_default(box.width, box.height);
        fl_path_decode(fl_entity_component(e, FL_COMPONENT_PATH), &path);
        if (cJSON_GetObjectItemCaseSensitive(props, "commands")) {
            Fl_Json json = {0}; Fl_Component decoded = {0};
            if (!fl_path_read(&json, props, &decoded) || !fl_path_decode(&decoded, &path)) {fl_component_clear(&decoded); return fl_fail(f, "invalid_argument", "%s: %s", ctx, json.error[0] ? json.error : "invalid path commands");}
            fl_component_clear(&decoded);
        }
        if (!agent_get_number(props, "trim_start", false, &path.trim_start, &present, f, ctx)) return false;
        if (!agent_get_number(props, "trim_end", false, &path.trim_end, &present, f, ctx)) return false;
        size_t size = fl_path_encode(&path, path_bytes, sizeof(path_bytes));
        if (!size) return fl_fail(f, "invalid_argument", "%s: path trim must stay inside 0..1", ctx);
        components[count++] = (Fl_Component){.type = FL_COMPONENT_PATH, .version = 1, .size = size, .data = path_bytes};
    }
    if (fl_entity_component(e, FL_COMPONENT_POST) || cJSON_GetObjectItemCaseSensitive(props, "vignette") || cJSON_GetObjectItemCaseSensitive(props, "grain") || cJSON_GetObjectItemCaseSensitive(props, "gamma")) {
        Fl_Post grade = fl_post_default();
        fl_post_decode(fl_entity_component(e, FL_COMPONENT_POST), &grade);
        static const char *const grade_keys[] = {"bloom", "vignette", "grain", "lift", "gamma", "gain"};
        double *grade_fields[] = {&grade.bloom, &grade.vignette, &grade.grain, &grade.lift, &grade.gamma, &grade.gain};
        for (size_t i = 0; i < 6; ++i) {
            if (!agent_get_number(props, grade_keys[i], false, &number, &present, f, ctx)) return false;
            if (present) *grade_fields[i] = number;
        }
        fl_post_encode(&grade, post_bytes);
        if (!fl_post_decode(&(Fl_Component){.type=FL_COMPONENT_POST,.version=1,.size=FL_POST_BYTES,.data=post_bytes}, &grade)) return fl_fail(f, "invalid_argument", "%s: grade is out of range", ctx);
        components[count++] = (Fl_Component){.type = FL_COMPONENT_POST, .version = 1, .size = FL_POST_BYTES, .data = post_bytes};
    }
    if (fl_entity_component(e, FL_COMPONENT_PARTICLES)) {
        Fl_Particles particles = fl_particles_default();
        fl_particles_decode(fl_entity_component(e, FL_COMPONENT_PARTICLES), &particles);
        static const char *const particle_keys[] = {"rate", "life", "size", "speed", "size_end", "gravity", "spread", "burst", "particle_blend", "force_x", "force_y", "force_z"};
        double *particle_fields[] = {&particles.rate, &particles.life, &particles.size, &particles.speed, &particles.size_end, &particles.gravity, &particles.spread, &particles.burst, &particles.blend, &particles.force_x, &particles.force_y, &particles.force_z};
        for (size_t i = 0; i < 12; ++i) {
            if (!agent_get_number(props, particle_keys[i], false, &number, &present, f, ctx)) return false;
            if (present) *particle_fields[i] = number;
        }
        if ((item = cJSON_GetObjectItemCaseSensitive(props, "color_end")) && !cJSON_IsNull(item) && !agent_parse_color(item, particles.color_end, f, "color_end")) return false;
        fl_particles_encode(&particles, particle_bytes);
        if (!fl_particles_decode(&(Fl_Component){.type=FL_COMPONENT_PARTICLES,.version=1,.size=FL_PARTICLES_BYTES,.data=particle_bytes}, &particles)) return fl_fail(f, "invalid_argument", "%s: particle values are out of range", ctx);
        components[count++] = (Fl_Component){.type = FL_COMPONENT_PARTICLES, .version = 1, .size = FL_PARTICLES_BYTES, .data = particle_bytes};
    }
    if (fl_entity_component(e, FL_COMPONENT_EFFECT) || cJSON_GetObjectItemCaseSensitive(props, "effect_kind") || cJSON_GetObjectItemCaseSensitive(props, "source")) {
        Fl_Effect effect = fl_effect_default();
        fl_effect_decode(fl_entity_component(e, FL_COMPONENT_EFFECT), &effect);
        if (!agent_get_number(props, "effect_kind", false, &number, &present, f, ctx)) return false;
        if (present) effect.kind = (uint32_t)number;
        static const char *const uniform_keys[] = {"u0", "u1", "u2", "u3", "u4", "u5", "u6", "u7"};
        for (int i = 0; i < 8; ++i) {
            if (!agent_get_number(props, uniform_keys[i], false, &number, &present, f, ctx)) return false;
            if (present) effect.u[i] = number;
        }
        const char *source = NULL;
        if (!agent_get_string(props, "source", false, &source, f, ctx)) return false;
        if (source) {effect.kind = FL_EFFECT_CUSTOM; effect.source = source; effect.source_size = strlen(source);}
        size_t size = fl_effect_encode(&effect, effect_bytes, sizeof(effect_bytes));
        if (!size) return fl_fail(f, "invalid_argument", "%s: effect is out of range", ctx);
        components[count++] = (Fl_Component){.type = FL_COMPONENT_EFFECT, .version = 1, .size = size, .data = effect_bytes};
    }
    return apply(x, &(Fl_Op){.kind = FL_OP_ENTITY_CONFIGURE, .scene = scene->id, .entity = e->id, .name = name, .visible = visible,
                             .components = components, .component_count = count}, NULL) || rejected(f, ctx, "property values");
}

static bool entity_create(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "scene", "kind", "parent", "asset", "props", NULL};
    const Fl_Scene *s; const char *kind_name; Fl_WidgetKind kind; Fl_Id parent = 0, asset = 0;
    if (!agent_only_keys(op, f, "entity.create", keys) || !(s = need_scene(x, op, "scene", f, "entity.create")) ||
        !agent_get_string(op, "kind", true, &kind_name, f, "entity.create") || !agent_get_id(op, "parent", &x->refs, false, &parent, f, "entity.create") ||
        !agent_get_id(op, "asset", &x->refs, false, &asset, f, "entity.create")) return false;
    if (!strcmp(kind_name, "camera") || !strcmp(kind_name, "plane") || !strcmp(kind_name, "cube") || !strcmp(kind_name, "sphere") || !strcmp(kind_name, "light") || !strcmp(kind_name, "particles")) kind = FL_WIDGET_COUNT;
    else if (!agent_widget_from_name(kind_name, &kind)) return fl_fail(f, "invalid_argument", "entity.create: kind must be entity, rectangle, ellipse, triangle, diamond, hexagon, star, line, arrow, text, image, video, path, effect, camera, plane, cube, sphere, light or particles");
    if (parent && !fl_scene_entity(s, parent)) return fl_fail(f, "not_found", "entity.create: parent %llu is not in scene %llu", (unsigned long long)parent, (unsigned long long)s->id);
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(op, "props");
    if (props && !agent_object(props, f, "entity.create props")) return false;
    double px = 0, py = 0, pz = 0;
    if (props && (!agent_get_number(props, "x", false, &px, NULL, f, "entity.create") || !agent_get_number(props, "y", false, &py, NULL, f, "entity.create") ||
        !agent_get_number(props, "z", false, &pz, NULL, f, "entity.create"))) return false;
    Fl_Id id = 0;
    if (kind == FL_WIDGET_COUNT) {
        if (!fl_spatial_create(x->project, s->id, parent, kind_name, px, py, pz, &id)) return rejected(f, "entity.create", "position must be finite");
    } else if (kind == FL_WIDGET_IMAGE) {
        const Fl_Asset *record = fl_project_asset(x->project, asset);
        if (!record || record->kind != FL_ASSET_IMAGE) return fl_fail(f, "not_found", "entity.create: image entities need \"asset\" to be an image asset id");
        double w = record->width ? (double)record->width : 320, h = record->height ? (double)record->height : 180;
        double fit = fmin(1, fmin(0.8 * (double)s->width / w, 0.8 * (double)s->height / h));
        if (props) {
            double v; bool present;
            if (!agent_get_number(props, "width", false, &v, &present, f, "entity.create")) return false;
            if (present) {w = v; fit = 1;}
            if (!agent_get_number(props, "height", false, &v, &present, f, "entity.create")) return false;
            if (present) {h = v; fit = 1;}
        }
        if (!(w > 0) || !(h > 0) || !fl_widget_create_image(x->project, s->id, parent, asset, px, py, w * fit, h * fit, &id)) return rejected(f, "entity.create", "image size or position");
    } else if (kind == FL_WIDGET_VIDEO) {
        const Fl_Asset *record = fl_project_asset(x->project, asset);
        if (!record || record->kind != FL_ASSET_VIDEO) return fl_fail(f, "not_found", "entity.create: video entities need \"asset\" to be a video asset id");
        double w = record->width ? (double)record->width : 1920, h = record->height ? (double)record->height : 1080, fit = fmin(1, fmin((double)s->width / w, (double)s->height / h));
        if (props) {
            double v; bool present;
            if (!agent_get_number(props, "width", false, &v, &present, f, "entity.create")) return false;
            if (present) {w = v; fit = 1;}
            if (!agent_get_number(props, "height", false, &v, &present, f, "entity.create")) return false;
            if (present) {h = v; fit = 1;}
        }
        if (!(w > 0) || !(h > 0) || !fl_widget_create_video(x->project, s->id, parent, asset, px, py, w * fit, h * fit, &id)) return rejected(f, "entity.create", "video size or position");
    } else if (!fl_widget_create(x->project, s->id, parent, kind, px, py, &id)) return rejected(f, "entity.create", "position must be finite");
    report_id(result, id);
    if (props && props->child) {
        const Fl_Entity *created = fl_scene_entity(fl_project_scene(x->project, s->id), id);
        if (!created || !configure(x, fl_project_scene(x->project, s->id), created, props, f, "entity.create props")) return false;
    }
    return true;
}

static bool entity_set(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "entity", "props", NULL};
    (void)result;
    const Fl_Scene *s; const Fl_Entity *e;
    if (!agent_only_keys(op, f, "entity.set", keys) || !(e = need_entity(x, op, "entity", &s, f, "entity.set"))) return false;
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(op, "props");
    if (!props) return fl_fail(f, "missing_field", "entity.set requires \"props\"");
    return configure(x, s, e, props, f, "entity.set");
}

static bool entity_remove(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "entity", NULL};
    (void)result;
    const Fl_Scene *s; const Fl_Entity *e;
    if (!agent_only_keys(op, f, "entity.remove", keys) || !(e = need_entity(x, op, "entity", &s, f, "entity.remove"))) return false;
    return apply(x, &(Fl_Op){.kind = FL_OP_ENTITY_REMOVE, .scene = s->id, .entity = e->id}, NULL) || rejected(f, "entity.remove", "entity");
}

static bool entity_reparent(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "entity", "parent", NULL};
    (void)result;
    const Fl_Scene *s; const Fl_Entity *e; Fl_Id parent;
    if (!agent_only_keys(op, f, "entity.reparent", keys) || !(e = need_entity(x, op, "entity", &s, f, "entity.reparent")) ||
        !agent_get_id(op, "parent", &x->refs, false, &parent, f, "entity.reparent")) return false;
    return apply(x, &(Fl_Op){.kind = FL_OP_ENTITY_PARENT, .scene = s->id, .entity = e->id, .parent = parent}, NULL) ||
           rejected(f, "entity.reparent", "the parent must be in the same scene and not a descendant of the entity");
}

static bool entity_reorder(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "entity", "position", NULL};
    (void)result;
    const Fl_Scene *s; const Fl_Entity *e; double position;
    if (!agent_only_keys(op, f, "entity.reorder", keys) || !(e = need_entity(x, op, "entity", &s, f, "entity.reorder")) ||
        !agent_get_number(op, "position", true, &position, NULL, f, "entity.reorder")) return false;
    return (position >= 0 && position < (double)s->entity_count &&
            apply(x, &(Fl_Op){.kind = FL_OP_ENTITY_MOVE, .scene = s->id, .entity = e->id, .position = (size_t)position}, NULL)) ||
           rejected(f, "entity.reorder", "position must be an index inside the scene's entity list (later entities draw on top)");
}

/* ---------- keyframes ---------- */

static bool key_set(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "entity", "property", "at", "value", "ease", "curve", "key", NULL};
    const Fl_Scene *s; const Fl_Entity *e; const char *property_name, *ease_name = "linear";
    Fl_AnimProperty property; Fl_Ease ease = FL_EASE_LINEAR; Fl_TimeAnchor anchor; Fl_Id key = 0;
    if (!agent_only_keys(op, f, "key.set", keys) || !(e = need_entity(x, op, "entity", &s, f, "key.set")) ||
        !agent_get_string(op, "property", true, &property_name, f, "key.set") || !agent_get_string(op, "ease", false, &ease_name, f, "key.set") ||
        !agent_get_id(op, "key", &x->refs, false, &key, f, "key.set")) return false;
    if (!agent_property_from_name(property_name, &property)) return fl_fail(f, "invalid_argument", "key.set: property must be a channel from get_reference");
    if (ease_name && !agent_ease_from_name(ease_name, &ease)) return fl_fail(f, "invalid_argument", "key.set: ease must be linear, in, out, in_out, hold, cubic or spring");
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(op, "at"), *value = cJSON_GetObjectItemCaseSensitive(op, "value");
    if (!at) return fl_fail(f, "missing_field", "key.set requires \"at\"");
    if (!value) return fl_fail(f, "missing_field", "key.set requires \"value\"");
    if (!agent_parse_anchor(at, &x->refs, &anchor, f, "key.set at")) return false;
    double v[4] = {0};
    const Fl_PropertyDef *channel = fl_property_definition(property);
    if (channel && channel->channels == 4) {
        unsigned char rgba[4];
        if (!agent_parse_color(value, rgba, f, "key.set value")) return false;
        for (int i = 0; i < 4; ++i) v[i] = rgba[i];
    } else {
        if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) return fl_fail(f, "invalid_argument", "key.set: value for %s must be a number", property_name);
        v[0] = property == FL_PROP_RX || property == FL_PROP_RY || property == FL_PROP_RZ || property == FL_PROP_GRADIENT_ANGLE ? value->valuedouble * DEG_TO_RAD : value->valuedouble;
    }
    double curve[4] = {0};
    const cJSON *curve_json = cJSON_GetObjectItemCaseSensitive(op, "curve");
    if (curve_json) {
        if (!cJSON_IsArray(curve_json) || cJSON_GetArraySize(curve_json) != 4) return fl_fail(f, "invalid_argument", "key.set: curve is four numbers");
        int i = 0;
        for (const cJSON *n = curve_json->child; n; n = n->next, ++i) {
            if (!cJSON_IsNumber(n) || !isfinite(n->valuedouble)) return fl_fail(f, "invalid_argument", "key.set: curve is four numbers");
            curve[i] = n->valuedouble;
        }
    }
    const Fl_WidgetDef *def = fl_widget_for_entity(e);
    bool crop_prop = property >= FL_PROP_CROP_X, text_prop = property == FL_PROP_FONT_SIZE, fill = property == FL_PROP_FILL, stroke = property == FL_PROP_STROKE || property == FL_PROP_STROKE_WIDTH;
    if (def && ((crop_prop && !(def->has_image || def->has_video)) || (text_prop && !def->has_text) || (fill && !def->has_fill) || (stroke && !def->has_stroke)))
        return fl_fail(f, "not_applicable", "key.set: a %s cannot animate %s", agent_widget_name(def->kind), property_name);
    Fl_Id created = 0;
    if (!apply(x, &(Fl_Op){.kind = FL_OP_KEY_SET, .scene = s->id, .entity = e->id, .key = key, .property = property, .anchor = anchor,
                           .value = {v[0], v[1], v[2], v[3]}, .ease = ease, .ease_param = {curve[0], curve[1], curve[2], curve[3]}}, &created))
        return rejected(f, "key.set", "the time must lie inside the scene (0 to its length) and the value inside the property's range");
    report_id(result, created);
    return true;
}

static bool key_remove(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "key", NULL};
    (void)result;
    Fl_Id id;
    if (!agent_only_keys(op, f, "key.remove", keys) || !agent_get_id(op, "key", &x->refs, true, &id, f, "key.remove")) return false;
    const Fl_Scene *s = agent_scene_of_key(x->project, id, NULL);
    if (!s) return fl_fail(f, "not_found", "key.remove: no keyframe with id %llu", (unsigned long long)id);
    return apply(x, &(Fl_Op){.kind = FL_OP_KEY_REMOVE, .scene = s->id, .key = id}, NULL) || rejected(f, "key.remove", "keyframe");
}

static bool track_remove(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "entity", "property", NULL};
    (void)result;
    const Fl_Scene *s; const Fl_Entity *e; const char *name; Fl_AnimProperty property;
    if (!agent_only_keys(op, f, "track.remove", keys) || !(e = need_entity(x, op, "entity", &s, f, "track.remove")) ||
        !agent_get_string(op, "property", true, &name, f, "track.remove")) return false;
    if (!agent_property_from_name(name, &property)) return fl_fail(f, "invalid_argument", "track.remove: unknown property \"%s\"", name);
    const Fl_Track *t = fl_scene_track(s, e->id, property);
    if (!t) return fl_fail(f, "not_found", "track.remove: %s has no %s animation", e->name, name);
    return apply(x, &(Fl_Op){.kind = FL_OP_TRACK_REMOVE, .scene = s->id, .track = t->id}, NULL) || rejected(f, "track.remove", "track");
}

/* ---------- beats ---------- */

static bool beat_add(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "scene", "name", "duration", "position", "auto_advance", NULL};
    const Fl_Scene *s; const char *name; Fl_Tick duration; double position; bool has_position, auto_advance = false;
    if (!agent_only_keys(op, f, "beat.add", keys) || !(s = need_scene(x, op, "scene", f, "beat.add")) ||
        !agent_get_string(op, "name", true, &name, f, "beat.add") || !agent_get_seconds(op, "duration", true, &duration, NULL, f, "beat.add") ||
        !agent_get_number(op, "position", false, &position, &has_position, f, "beat.add") || !agent_get_bool(op, "auto_advance", &auto_advance, NULL, f, "beat.add")) return false;
    if (!has_position) position = (double)s->beat_count;
    Fl_Id id = 0;
    if (duration <= 0 || !name[0] || position < 0 || position > (double)s->beat_count ||
        !apply(x, &(Fl_Op){.kind = FL_OP_BEAT_ADD, .scene = s->id, .name = name, .position = (size_t)position, .beat_duration = (uint64_t)duration, .auto_advance = auto_advance}, &id))
        return rejected(f, "beat.add", "name must be non-empty, duration positive, position within 0..beat count");
    report_id(result, id); return true;
}

static bool beat_update(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "beat", "name", "duration", "auto_advance", NULL};
    (void)result;
    Fl_Id id;
    if (!agent_only_keys(op, f, "beat.update", keys) || !agent_get_id(op, "beat", &x->refs, true, &id, f, "beat.update")) return false;
    const Fl_Scene *s = agent_scene_of_beat(x->project, id);
    if (!s) return fl_fail(f, "not_found", "beat.update: no beat with id %llu", (unsigned long long)id);
    const Fl_Beat *beat = NULL;
    for (size_t i = 0; i < s->beat_count; ++i) if (s->beats[i].id == id) beat = &s->beats[i];
    const char *name = beat->name; Fl_Tick duration = (Fl_Tick)beat->duration; bool auto_advance = beat->auto_advance, present;
    if (!agent_get_string(op, "name", false, &name, f, "beat.update") || !agent_get_seconds(op, "duration", false, &duration, &present, f, "beat.update") ||
        !agent_get_bool(op, "auto_advance", &auto_advance, NULL, f, "beat.update")) return false;
    if (!name) name = beat->name;
    return (duration > 0 && name[0] &&
            apply(x, &(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .scene = s->id, .beat = id, .name = name, .beat_duration = (uint64_t)duration, .auto_advance = auto_advance}, NULL)) ||
           rejected(f, "beat.update", "name must be non-empty and duration positive; keyframes inside the beat scale with it");
}

static bool beat_remove(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "beat", NULL};
    (void)result;
    Fl_Id id;
    if (!agent_only_keys(op, f, "beat.remove", keys) || !agent_get_id(op, "beat", &x->refs, true, &id, f, "beat.remove")) return false;
    const Fl_Scene *s = agent_scene_of_beat(x->project, id);
    if (!s) return fl_fail(f, "not_found", "beat.remove: no beat with id %llu", (unsigned long long)id);
    return apply(x, &(Fl_Op){.kind = FL_OP_BEAT_REMOVE, .scene = s->id, .beat = id}, NULL) || rejected(f, "beat.remove", "a scene keeps at least one beat");
}

static bool beat_move(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "beat", "position", NULL};
    (void)result;
    Fl_Id id; double position;
    if (!agent_only_keys(op, f, "beat.move", keys) || !agent_get_id(op, "beat", &x->refs, true, &id, f, "beat.move") ||
        !agent_get_number(op, "position", true, &position, NULL, f, "beat.move")) return false;
    const Fl_Scene *s = agent_scene_of_beat(x->project, id);
    if (!s) return fl_fail(f, "not_found", "beat.move: no beat with id %llu", (unsigned long long)id);
    return (position >= 0 && position < (double)s->beat_count &&
            apply(x, &(Fl_Op){.kind = FL_OP_BEAT_MOVE, .scene = s->id, .beat = id, .position = (size_t)position}, NULL)) ||
           rejected(f, "beat.move", "position must be an index inside the beat list");
}

static bool beat_split(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "beat", "at", "name", NULL};
    Fl_Id id; Fl_Tick at; const char *name;
    if (!agent_only_keys(op, f, "beat.split", keys) || !agent_get_id(op, "beat", &x->refs, true, &id, f, "beat.split") ||
        !agent_get_seconds(op, "at", true, &at, NULL, f, "beat.split") || !agent_get_string(op, "name", true, &name, f, "beat.split")) return false;
    const Fl_Scene *s = agent_scene_of_beat(x->project, id);
    if (!s) return fl_fail(f, "not_found", "beat.split: no beat with id %llu", (unsigned long long)id);
    Fl_Id created = 0;
    if (!apply(x, &(Fl_Op){.kind = FL_OP_BEAT_SPLIT, .scene = s->id, .beat = id, .tick = at, .name = name}, &created))
        return rejected(f, "beat.split", "\"at\" is scene time in seconds and must fall strictly inside the beat; name must be non-empty");
    report_id(result, created); return true;
}

/* ---------- audio clips ---------- */

/* Parse clip fields over a base record (a new clip's defaults or the clip being updated). */
static bool clip_fields(Ctx *x, const cJSON *op, Fl_AudioClip *clip, Fl_Fail *f, const char *ctx)
{
    bool present; double gain;
    Fl_Tick t;
    const cJSON *start = cJSON_GetObjectItemCaseSensitive(op, "start");
    if (start && !agent_parse_anchor(start, &x->refs, &clip->start, f, "clip start")) return false;
    if (!agent_get_seconds(op, "source_offset", false, &t, &present, f, ctx)) return false;
    if (present) {if (t < 0) return fl_fail(f, "invalid_argument", "%s: source_offset cannot be negative", ctx); clip->source_offset = (uint64_t)t;}
    if (!agent_get_seconds(op, "duration", false, &t, &present, f, ctx)) return false;
    if (present) {if (t < 0) return fl_fail(f, "invalid_argument", "%s: duration cannot be negative", ctx); clip->duration = (uint64_t)t;}
    if (!agent_get_seconds(op, "fade_in", false, &t, &present, f, ctx)) return false;
    if (present) {if (t < 0) return fl_fail(f, "invalid_argument", "%s: fade_in cannot be negative", ctx); clip->fade_in = (uint64_t)t;}
    if (!agent_get_seconds(op, "fade_out", false, &t, &present, f, ctx)) return false;
    if (present) {if (t < 0) return fl_fail(f, "invalid_argument", "%s: fade_out cannot be negative", ctx); clip->fade_out = (uint64_t)t;}
    if (!agent_get_number(op, "gain", false, &gain, &present, f, ctx)) return false;
    if (present) clip->gain = gain;
    return true;
}

static bool clip_add(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "scene", "asset", "start", "source_offset", "duration", "gain", "fade_in", "fade_out", NULL};
    const Fl_Scene *s; Fl_AudioClip clip = {.gain = 1};
    if (!agent_only_keys(op, f, "clip.add", keys) || !(s = need_scene(x, op, "scene", f, "clip.add")) ||
        !agent_get_id(op, "asset", &x->refs, true, &clip.asset, f, "clip.add") || !clip_fields(x, op, &clip, f, "clip.add")) return false;
    Fl_Id id = 0;
    if (!apply(x, &(Fl_Op){.kind = FL_OP_CLIP_ADD, .scene = s->id, .clip_record = &clip}, &id))
        return rejected(f, "clip.add", "asset must be an audio asset; gain is 0..4; the clip must start inside the scene");
    report_id(result, id); return true;
}

static bool clip_update(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "clip", "start", "source_offset", "duration", "gain", "fade_in", "fade_out", NULL};
    (void)result;
    Fl_Id id;
    if (!agent_only_keys(op, f, "clip.update", keys) || !agent_get_id(op, "clip", &x->refs, true, &id, f, "clip.update")) return false;
    const Fl_Scene *s = agent_scene_of_clip(x->project, id);
    if (!s) return fl_fail(f, "not_found", "clip.update: no clip with id %llu", (unsigned long long)id);
    Fl_AudioClip clip = {0};
    for (size_t i = 0; i < s->clip_count; ++i) if (s->clips[i].id == id) clip = s->clips[i];
    return clip_fields(x, op, &clip, f, "clip.update") &&
           (apply(x, &(Fl_Op){.kind = FL_OP_CLIP_UPDATE, .scene = s->id, .clip = id, .clip_record = &clip}, NULL) || rejected(f, "clip.update", "clip values"));
}

static bool clip_remove(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "clip", NULL};
    (void)result;
    Fl_Id id;
    if (!agent_only_keys(op, f, "clip.remove", keys) || !agent_get_id(op, "clip", &x->refs, true, &id, f, "clip.remove")) return false;
    const Fl_Scene *s = agent_scene_of_clip(x->project, id);
    if (!s) return fl_fail(f, "not_found", "clip.remove: no clip with id %llu", (unsigned long long)id);
    return apply(x, &(Fl_Op){.kind = FL_OP_CLIP_REMOVE, .scene = s->id, .clip = id}, NULL) || rejected(f, "clip.remove", "clip");
}

/* ---------- scripts ---------- */

/* Resolve a project Lua asset by its typed identifier. */
static const Fl_Asset *need_script(Ctx *x, const cJSON *op, Fl_Fail *f, const char *ctx)
{
    Fl_Id id;
    if (!agent_get_id(op, "script", &x->refs, true, &id, f, ctx)) return NULL;
    const Fl_Asset *asset = fl_project_asset(x->project, id);
    if (!asset || asset->kind != FL_ASSET_LUA) {
        fl_fail(f, "not_found", "%s: no Lua asset with id %llu", ctx, (unsigned long long)id);
        return NULL;
    }
    return asset;
}

/* Capture reusable authored content through the shared native authoring owner. */
static bool prototype_capture(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[]={"op","as","entity","name",NULL};
    const Fl_Scene *scene; const Fl_Entity *entity; const char *name=NULL; Fl_Id id=0;
    if (!agent_only_keys(op,f,"prototype.capture",keys) || !(entity=need_entity(x,op,"entity",&scene,f,"prototype.capture")) ||
        !agent_get_string(op,"name",true,&name,f,"prototype.capture")) return false;
    if (!fl_prototype_capture(x->project,scene->id,entity->id,name,&id)) return rejected(f,"prototype.capture","invalid hierarchy or prototype name");
    report_id(result,id); return true;
}

/* Place a live instance at a native scene position. */
static bool prototype_instantiate(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[]={"op","as","scene","parent","asset","x","y",NULL};
    const Fl_Scene *scene; Fl_Id parent=0,asset=0,id=0; double px=0,py=0;
    if (!agent_only_keys(op,f,"prototype.instantiate",keys) || !(scene=need_scene(x,op,"scene",f,"prototype.instantiate")) ||
        !agent_get_id(op,"parent",&x->refs,false,&parent,f,"prototype.instantiate") || !agent_get_id(op,"asset",&x->refs,true,&asset,f,"prototype.instantiate") ||
        !agent_get_number(op,"x",false,&px,NULL,f,"prototype.instantiate") || !agent_get_number(op,"y",false,&py,NULL,f,"prototype.instantiate")) return false;
    if (!fl_prototype_instantiate(x->project,scene->id,parent,asset,px,py,&id)) return rejected(f,"prototype.instantiate","prototype resource, parent or placement");
    report_id(result,id); return true;
}

/* Replace an instance's explicit overrides; native validation and history apply to the entire batch. */
static bool prototype_overrides(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    (void)result; static const char *const keys[]={"op","as","entity","value",NULL};
    const Fl_Scene *scene; const Fl_Entity *entity;
    if (!agent_only_keys(op,f,"prototype.overrides",keys) || !(entity=need_entity(x,op,"entity",&scene,f,"prototype.overrides"))) return false;
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(op,"value");
    if (!cJSON_IsArray(value)) return fl_fail(f,"invalid_argument","prototype.overrides requires an operation array in value");
    char *text=cJSON_PrintUnformatted(value);
    bool ok=text && fl_prototype_set_overrides(x->project,scene->id,entity->id,text,strlen(text)); free(text);
    return ok || rejected(f,"prototype.overrides",fl_project_diagnostic(x->project)[0] ? fl_project_diagnostic(x->project) : "invalid prototype override");
}

/* Detach into scene-owned native content while retaining generator identity and scoped clocks. */
static bool prototype_detach(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    (void)result; static const char *const keys[]={"op","as","entity",NULL};
    const Fl_Scene *scene; const Fl_Entity *entity;
    if (!agent_only_keys(op,f,"prototype.detach",keys) || !(entity=need_entity(x,op,"entity",&scene,f,"prototype.detach"))) return false;
    return fl_prototype_detach(x->project,scene->id,entity->id) || rejected(f,"prototype.detach","entity has no detachable prototype or already owns a composition");
}

/* Apply one provenance-addressed edit without exposing evaluated IDs as persistent identities. */
static bool scope_edit(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    (void)result; static const char *const keys[]={"op","as","entity","component","target","path","value",NULL};
    const Fl_Scene *scene; const Fl_Entity *owner; Fl_Origin address={.generated=true};
    if (!agent_only_keys(op,f,"scope.edit",keys) || !(owner=need_entity(x,op,"entity",&scene,f,"scope.edit")) ||
        !agent_get_id(op,"component",&x->refs,true,&address.component,f,"scope.edit") || !agent_get_id(op,"target",&x->refs,true,&address.entity,f,"scope.edit")) return false;
    address.owner=owner->id; const cJSON *path=cJSON_GetObjectItemCaseSensitive(op,"path");
    if (path && (!cJSON_IsArray(path) || cJSON_GetArraySize(path)>32)) return fl_fail(f,"invalid_argument","scope.edit path must be an array of at most32 component IDs");
    for (const cJSON *part=path ? path->child : NULL;part;part=part->next)
        if (!agent_id_value(part,&x->refs,&address.path[address.depth++],f,"scope.edit")) return false;
    return fl_scope_edit(x->project,scene->id,&address,cJSON_GetObjectItemCaseSensitive(op,"value")) || rejected(f,"scope.edit",fl_project_diagnostic(x->project)[0] ? fl_project_diagnostic(x->project) : "invalid scoped edit");
}

/* Author editable asset bytes through the same validated document changeset as native editing. */
static bool asset_source(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[]={"op","as","asset","kind","name","source",NULL};
    const char *kind=NULL,*name=NULL,*source=NULL; Fl_Id asset=0,id=0;
    if (!agent_only_keys(op,f,"asset.source",keys) || !agent_get_id(op,"asset",&x->refs,false,&asset,f,"asset.source") ||
        !agent_get_string(op,"kind",true,&kind,f,"asset.source") || !agent_get_string(op,"name",false,&name,f,"asset.source") ||
        !agent_get_string(op,"source",false,&source,f,"asset.source")) return false;
    Fl_AssetKind type=!strcmp(kind,"lua") ? FL_ASSET_LUA : !strcmp(kind,"flproto") ? FL_ASSET_PROTOTYPE : 0;
    if (!type) return fl_fail(f,"invalid_argument","asset.source: kind must be lua or flproto");
    if (!apply(x,&(Fl_Op){.kind=FL_OP_ASSET_SOURCE_SET,.source_kind=type,.asset=asset,.name=name,.source=source,.source_size=source ? strlen(source) : 0},&id))
        return rejected(f,"asset.source",fl_project_diagnostic(x->project)[0] ? fl_project_diagnostic(x->project) : "invalid source asset changeset");
    report_id(result,id ? id : asset); return true;
}

/* Rename or delete an ordinary project asset; referenced assets cannot be removed. */
static bool asset_edit(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    (void)result;
    static const char *const rename_keys[]={"op","as","asset","name",NULL}, *const remove_keys[]={"op","as","asset",NULL};
    const char *action=cJSON_GetObjectItemCaseSensitive(op,"op")->valuestring,*name=NULL; Fl_Id asset=0;
    bool rename=!strcmp(action,"asset.rename");
    if (!agent_only_keys(op,f,action,rename ? rename_keys : remove_keys) || !agent_get_id(op,"asset",&x->refs,true,&asset,f,action) ||
        (rename && !agent_get_string(op,"name",true,&name,f,action))) return false;
    return apply(x,&(Fl_Op){.kind=rename ? FL_OP_ASSET_RENAME : FL_OP_ASSET_REMOVE,.asset=asset,.name=name},NULL) || rejected(f,action,"asset is missing, referenced or has an invalid name");
}

static bool script_create(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "name", "source", NULL};
    const char *name, *source;
    if (!agent_only_keys(op, f, "script.create", keys) ||
        !agent_get_string(op, "name", true, &name, f, "script.create") || !agent_get_string(op, "source", true, &source, f, "script.create")) return false;
    Fl_Id id = 0;
    if (!apply(x, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .name = name, .source = source, .source_size = strlen(source)}, &id))
        return rejected(f,"script.create",fl_project_diagnostic(x->project)[0] ? fl_project_diagnostic(x->project) : "name must be unique; source must be bounded valid Lua");
    report_id(result, id);
    return true;
}

/* Apply the {old, new} edits to text, each `old` matching exactly once; returns an owned result or NULL with f set. */
static char *edited_source(const char *text, size_t size, const cJSON *edits, size_t *out_size, Fl_Fail *f)
{
    if (!cJSON_IsArray(edits) || !cJSON_GetArraySize(edits)) {fl_fail(f, "invalid_argument", "script.update: edits must be a non-empty array of {old, new}"); return NULL;}
    char *current = malloc(size + 1);
    if (!current) {fl_fail(f, "internal", "out of memory"); return NULL;}
    memcpy(current, text, size); current[size] = 0;
    size_t length = size, index = 0;
    for (const cJSON *edit = edits->child; edit; edit = edit->next, ++index) {
        const cJSON *from = cJSON_GetObjectItemCaseSensitive(edit, "old"), *to = cJSON_GetObjectItemCaseSensitive(edit, "new");
        if (!cJSON_IsString(from) || !from->valuestring[0] || !cJSON_IsString(to)) {
            fl_fail(f, "invalid_argument", "script.update: edits[%zu] needs a non-empty string old and a string new", index); free(current); return NULL;
        }
        const char *match = strstr(current, from->valuestring), *again = match ? strstr(match + 1, from->valuestring) : NULL;
        if (!match || again) {
            fl_fail(f, "invalid_argument", "script.update: edits[%zu].old %s", index, match ? "occurs more than once; include more context" : "was not found in the script"); free(current); return NULL;
        }
        size_t old_size = strlen(from->valuestring), new_size = strlen(to->valuestring), head = (size_t)(match - current);
        char *next = malloc(length - old_size + new_size + 1);
        if (!next) {fl_fail(f, "internal", "out of memory"); free(current); return NULL;}
        memcpy(next, current, head); memcpy(next + head, to->valuestring, new_size);
        memcpy(next + head + new_size, current + head + old_size, length - head - old_size + 1);
        free(current); current = next; length = length - old_size + new_size;
    }
    *out_size = length;
    return current;
}

static bool script_update(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    (void)result;
    static const char *const keys[] = {"op", "as", "script", "name", "source", "edits", NULL};
    const Fl_Asset *script; const char *name = NULL, *source = NULL;
    if (!agent_only_keys(op, f, "script.update", keys) || !(script = need_script(x, op, f, "script.update")) ||
        !agent_get_string(op, "name", false, &name, f, "script.update") || !agent_get_string(op, "source", false, &source, f, "script.update")) return false;
    const cJSON *edits = cJSON_GetObjectItemCaseSensitive(op, "edits");
    if (source && edits) return fl_fail(f, "invalid_argument", "script.update: give source or edits, not both");
    if (!name && !source && !edits) return fl_fail(f, "missing_field", "script.update: give name, source or edits");
    char *patched = NULL; size_t size = source ? strlen(source) : 0;
    if (edits && !(patched = edited_source(script->source, script->size, edits, &size, f))) return false;
    const char *text = patched ? patched : source;
    bool ok = apply(x, &(Fl_Op){.kind = FL_OP_ASSET_SOURCE_SET, .source_kind=FL_ASSET_LUA, .asset = script->id, .name = name, .source = text, .source_size = size}, NULL);
    free(patched);
    return ok || rejected(f,"script.update",fl_project_diagnostic(x->project)[0] ? fl_project_diagnostic(x->project) : "name must be unique; source must be bounded valid Lua");
}

static bool script_remove(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "script", NULL};
    (void)result;
    const Fl_Asset *script;
    if (!agent_only_keys(op, f, "script.remove", keys) || !(script = need_script(x, op, f, "script.remove"))) return false;
    return apply(x, &(Fl_Op){.kind = FL_OP_ASSET_REMOVE, .asset = script->id}, NULL) || rejected(f, "script.remove", "script");
}

static bool script_attach(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "entity", "script", "enabled", NULL};
    (void)result;
    const Fl_Scene *s; const Fl_Entity *e; bool enabled = true;
    if (!agent_only_keys(op, f, "script.attach", keys) || !(e = need_entity(x, op, "entity", &s, f, "script.attach")) ||
        !agent_get_bool(op, "enabled", &enabled, NULL, f, "script.attach")) return false;
    Fl_Id id;
    if (!agent_get_id(op, "script", &x->refs, true, &id, f, "script.attach")) return false;
    if (!fl_project_asset(s->project, id)) return fl_fail(f, "not_found", "script.attach: the scene of %s has no script with id %llu", e->name, (unsigned long long)id);
    Fl_Component component = {0};
    if (!fl_script_encode(&(Fl_Script){.script = id, .enabled = enabled}, &component)) return fl_fail(f, "internal", "script.attach: out of memory");
    const Fl_Component *existing=fl_entity_script_component(e,id);
    if (existing) component.id=existing->id;
    bool ok = apply(x, &(Fl_Op){.kind = FL_OP_COMPONENT_SET, .scene = s->id, .entity = e->id, .component = component}, NULL);
    free((void *)component.data);
    return ok || rejected(f, "script.attach", "script component");
}

static bool script_detach(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[] = {"op", "as", "entity", NULL};
    (void)result;
    const Fl_Scene *s; const Fl_Entity *e;
    if (!agent_only_keys(op, f, "script.detach", keys) || !(e = need_entity(x, op, "entity", &s, f, "script.detach"))) return false;
    return apply(x, &(Fl_Op){.kind = FL_OP_COMPONENT_REMOVE, .scene = s->id, .entity = e->id, .component = {.type = FL_COMPONENT_SCRIPT}}, NULL) ||
           fl_fail(f, "not_found", "script.detach: %s has no script", e->name);
}

/* Set a complete typed component instance using the same schema as project files. */
static bool component_set(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    static const char *const keys[]={"op","as","entity","value",NULL};
    const Fl_Scene *scene; const Fl_Entity *entity;
    if (!agent_only_keys(op,f,"component.set",keys) || !(entity=need_entity(x,op,"entity",&scene,f,"component.set"))) return false;
    Fl_Json context={0}; Fl_Component component={0};
    bool decoded=fl_document_component_read(&context,cJSON_GetObjectItemCaseSensitive(op,"value"),entity->id,&component);
    if (!decoded) {fl_component_clear(&component); return fl_fail(f,"invalid_argument","component.set: %s",context.error);}
    Fl_Id id=0; bool ok=apply(x,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene->id,.entity=entity->id,.component=component},&id);
    fl_component_clear(&component);
    if (ok) report_id(result,id);
    return ok || rejected(f,"component.set","component identity, schema or binding");
}

/* Remove exactly one optional component by its stable instance identity. */
static bool component_remove(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    (void)result; static const char *const keys[]={"op","as","entity","component",NULL};
    const Fl_Scene *scene; const Fl_Entity *entity; Fl_Id id;
    if (!agent_only_keys(op,f,"component.remove",keys) || !(entity=need_entity(x,op,"entity",&scene,f,"component.remove")) ||
        !agent_get_id(op,"component",&x->refs,true,&id,f,"component.remove")) return false;
    return apply(x,&(Fl_Op){.kind=FL_OP_COMPONENT_REMOVE,.scene=scene->id,.entity=entity->id,.component.id=id},NULL) || rejected(f,"component.remove","component instance");
}

static const struct {const char *name; OpFn run;} OPERATIONS[] = {
    {"scene.add", scene_add}, {"scene.remove", scene_remove}, {"scene.rename", scene_rename}, {"scene.move", scene_move}, {"scene.set", scene_set},
    {"entity.create", entity_create}, {"entity.set", entity_set}, {"entity.remove", entity_remove}, {"entity.reparent", entity_reparent},
    {"entity.reorder", entity_reorder},
    {"key.set", key_set}, {"key.remove", key_remove}, {"track.remove", track_remove},
    {"beat.add", beat_add}, {"beat.update", beat_update}, {"beat.remove", beat_remove}, {"beat.move", beat_move}, {"beat.split", beat_split},
    {"clip.add", clip_add}, {"clip.update", clip_update}, {"clip.remove", clip_remove},
    {"prototype.capture",prototype_capture}, {"prototype.instantiate",prototype_instantiate}, {"prototype.overrides",prototype_overrides}, {"prototype.detach",prototype_detach}, {"scope.edit",scope_edit},
    {"asset.source", asset_source}, {"asset.rename", asset_edit}, {"asset.remove", asset_edit},
    {"script.create", script_create}, {"script.update", script_update}, {"script.remove", script_remove},
    {"script.attach", script_attach}, {"script.detach", script_detach},
    {"component.set",component_set}, {"component.remove",component_remove},
};

/* Run one operation and register its `as` name; the result object already holds the op name. */
static bool run_op(Ctx *x, const cJSON *op, cJSON *result, Fl_Fail *f)
{
    const char *name;
    if (!agent_object(op, f, "operation") || !agent_get_string(op, "op", true, &name, f, "operation")) return false;
    cJSON_AddStringToObject(result, "op", name);
    const char *as = NULL;
    if (!agent_get_string(op, "as", false, &as, f, name)) return false;
    if (as && (as[0] != '$' || strlen(as) < 2 || strlen(as) >= sizeof(x->refs.names[0]) || x->refs.count >= FL_AGENT_MAX_REFS))
        return fl_fail(f, "invalid_argument", "%s: \"as\" must be \"$name\" (at most 46 characters)", name);
    for (size_t i = 0; as && i < x->refs.count; ++i) if (!strcmp(x->refs.names[i], as)) return fl_fail(f, "invalid_argument", "%s: name %s is already used in this batch", name, as);
    for (size_t i = 0; i < sizeof(OPERATIONS) / sizeof(*OPERATIONS); ++i) {
        if (strcmp(OPERATIONS[i].name, name)) continue;
        if (!OPERATIONS[i].run(x, op, result, f)) return false;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(result, "id");
        if (as) {
            if (!id) return fl_fail(f, "invalid_argument", "%s creates nothing, so \"as\" has no id to name", name);
            snprintf(x->refs.names[x->refs.count], sizeof(x->refs.names[0]), "%s", as);
            if (!agent_id_value(id, NULL, &x->refs.ids[x->refs.count], f, name)) return false;
            ++x->refs.count;
            cJSON_AddStringToObject(result, "as", as);
        }
        return true;
    }
    return fl_fail(f, "unknown_operation", "unknown operation \"%s\"", name);
}

/* Scene id -> revision pairs the caller expects to still be current. */
static bool check_expectations(Fl_Project *p, const cJSON *expect, cJSON **error)
{
    if (!expect) return true;
    Fl_Fail fail;
    if (!cJSON_IsObject(expect)) {fl_fail(&fail, "invalid_argument", "expect must map scene ids to revisions"); *error = fl_fail_json(&fail); return false;}
    cJSON *stale = NULL;
    for (const cJSON *item = expect->child; item; item = item->next) {
        Fl_Id id, revision; Fl_Json json = {0};
        cJSON *key = cJSON_CreateString(item->string);
        bool valid = key && fl_json_uint(&json, key, &id) && id && fl_json_uint(&json, item, &revision) && revision;
        cJSON_Delete(key);
        const Fl_Scene *s = valid ? fl_project_scene(p, id) : NULL;
        if (!s) {fl_fail(&fail, "invalid_argument", "expect: %s requires an exact scene id and revision", item->string); cJSON_Delete(stale); *error = fl_fail_json(&fail); return false;}
        if (s->revision != revision) {
            if (!stale) stale = cJSON_CreateArray();
            if (stale) cJSON_AddItemToArray(stale, agent_scene_summary(s));
        }
    }
    if (!stale) return true;
    fl_fail(&fail, "revision_conflict", "A scene changed since you read it. Re-read the listed scenes and rebuild the changeset; nothing was applied.");
    *error = fl_fail_json(&fail);
    if (*error) cJSON_AddItemToObject(*error, "changed_scenes", stale); else cJSON_Delete(stale);
    return false;
}

cJSON *agent_tool_apply_ops(Fl_Agent *agent, const cJSON *args, cJSON **error)
{
    static const char *const keys[] = {"ops", "label", "expect", "dry_run", NULL};
    Fl_Fail fail;
    Fl_Project *project = agent->host.project(agent->host.user_data);
    if (!project) {fl_fail(&fail, "no_project", "No project is open in Flow"); *error = fl_fail_json(&fail); return NULL;}
    const cJSON *ops = cJSON_GetObjectItemCaseSensitive(args, "ops");
    const char *label = NULL;
    bool dry_run = false;
    if (!agent_only_keys(args, &fail, "apply_ops", keys) || !agent_get_string(args, "label", false, &label, &fail, "apply_ops") ||
        !agent_get_bool(args, "dry_run", &dry_run, NULL, &fail, "apply_ops")) {*error = fl_fail_json(&fail); return NULL;}
    if (!cJSON_IsArray(ops) || !cJSON_GetArraySize(ops) || (unsigned)cJSON_GetArraySize(ops) > FL_AGENT_MAX_OPS) {
        fl_fail(&fail, "invalid_argument", "apply_ops: \"ops\" must be an array of 1..%u operations", FL_AGENT_MAX_OPS);
        *error = fl_fail_json(&fail); return NULL;
    }
    if (!check_expectations(project, cJSON_GetObjectItemCaseSensitive(args, "expect"), error)) return NULL;
    Ctx x = {.project = project};
    x.refs.names = calloc(FL_AGENT_MAX_REFS, sizeof(*x.refs.names)); x.refs.ids = calloc(FL_AGENT_MAX_REFS, sizeof(*x.refs.ids));
    cJSON *results = cJSON_CreateArray();
    if (!x.refs.names || !x.refs.ids || !results || !fl_project_edit_begin(project,label,false)) {
        free(x.refs.names); free(x.refs.ids); cJSON_Delete(results);
        fl_fail(&fail, "internal", "out of memory preparing the changeset"); *error = fl_fail_json(&fail); return NULL;
    }
    uint64_t start = fl_project_generation(project);
    int index = 0;
    bool ok = true;
    cJSON *failing_op = NULL;
    for (const cJSON *op = ops->child; op && ok; op = op->next, ++index) {
        cJSON *result = cJSON_CreateObject();
        if (!result) {ok = fl_fail(&fail, "internal", "out of memory"); break;}
        ok = run_op(&x, op, result, &fail);
        if (ok) cJSON_AddItemToArray(results, result);
        else {failing_op = result; break;}
    }
    cJSON *out = NULL;
    if (!ok) {
        if (!fl_project_edit_cancel(project)) fl_fail(&fail,"internal","failed to cancel document changeset");
        *error = fl_fail_json(&fail);
        if (*error) {
            cJSON_AddNumberToObject(*error, "op_index", index);
            const cJSON *name = failing_op ? cJSON_GetObjectItemCaseSensitive(failing_op, "op") : NULL;
            if (name) cJSON_AddStringToObject(*error, "op", name->valuestring);
            cJSON_AddStringToObject(*error, "state", "nothing was applied");
        }
        cJSON_Delete(failing_op); cJSON_Delete(results);
    } else {
        bool changed = fl_project_generation(project) != start;
        bool valid=!dry_run || fl_project_edit_validate(project);
        bool finished=changed && !dry_run ? fl_project_edit_commit(project) : fl_project_edit_cancel(project);
        bool completed=valid && finished;
        if (!completed) {
            cJSON_Delete(results); free(x.refs.names); free(x.refs.ids);
            fl_fail(&fail,"rejected","%s",fl_project_diagnostic(project)[0] ? fl_project_diagnostic(project) : "failed to complete document changeset"); *error=fl_fail_json(&fail); return NULL;
        }
        out = cJSON_CreateObject();
        cJSON *scenes = out ? cJSON_AddArrayToObject(out, "scenes") : NULL;
        if (out) {
            cJSON_AddBoolToObject(out, "dry_run", dry_run);
            cJSON_AddNumberToObject(out, "applied", index);
            cJSON_AddItemToObject(out, "results", results);
            for (size_t i = 0; scenes && i < fl_project_scene_count(project); ++i) cJSON_AddItemToArray(scenes, agent_scene_summary(fl_project_scene_at(project, i)));
            cJSON_AddNumberToObject(out, "undo_available", (double)fl_history_undo_count(fl_project_history(project)));
        } else cJSON_Delete(results);
        if (out && !dry_run) {
            agent->touched_count = 0;
            for (const cJSON *op = ops->child; op; op = op->next) {
                Fl_Id id = 0; Fl_Fail ignore = {0};
                if (!agent_get_id(op, "entity", &x.refs, false, &id, &ignore, "apply_ops") || !id) continue;
                size_t n = 0;
                for (; n < agent->touched_count; ++n) if (agent->touched[n] == id) break;
                if (n == agent->touched_count && agent->touched_count < 64) agent->touched[agent->touched_count++] = id;
            }
            for (const cJSON *result = results ? results->child : NULL; result; result = result->next) {
                Fl_Id id = 0; Fl_Fail ignore = {0};
                const cJSON *idj = cJSON_GetObjectItemCaseSensitive(result, "id");
                if (!idj || !agent_id_value(idj, &x.refs, &id, &ignore, "apply_ops") || !id) continue;
                size_t n = 0;
                for (; n < agent->touched_count; ++n) if (agent->touched[n] == id) break;
                if (n == agent->touched_count && agent->touched_count < 64) agent->touched[agent->touched_count++] = id;
            }
        }
    }
    free(x.refs.names); free(x.refs.ids);
    return out;
}

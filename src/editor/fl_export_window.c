#include "fl_export_window.h"
#include "fl_file_browser.h"
#include "ui/fl_button.h"
#include "style.h"
#include <ca_dock.h>
#include "platform/fl_fs.h"
#include "platform/fl_process.h"
#include "render/fl_frame.h"
#include "time/fl_time.h"
#include "core/fl_clock.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FL_EXPORT_CARDS 16
#define FL_EXPORT_STRIP_H 96.f
#define FL_EXPORT_CARD_H 62.f

/* One captured frame. The glass and the picture are the same box; the strip moves them together. */
typedef struct Fl_ExportCard {
    Ca_Image *image;
    uint32_t src_w, src_h;
    uint64_t id;
} Fl_ExportCard;

typedef struct Fl_ExportScene {
    struct Fl_ExportWindow *window;
    Fl_Id id;
    bool selected;
    char name[96];
    char box_id[40];
} Fl_ExportScene;

struct Fl_ExportWindow {
    Ca_Instance *instance;
    Fl_Project *project;
    Fl_Exporter *exporter;
    Fl_FileBrowser *browser;
    Ca_Dock *dock;
    Ca_Div *stage;
    Ca_Effect *watch;
    Fl_ExportCard cards[FL_EXPORT_CARDS];
    int card_count;
    uint64_t next_card_id;
    uint64_t last_preview_ns, interval_ns;
    float stage_w, stage_h, layout_w;
    uint64_t image_sequence;
    char file_name[256];
    char folder[FL_PATH_MAX];
    char width[16], fps[16], crf[16], crossfade[32];
    char error[256];
    Fl_ExportScene *scenes;
    size_t scene_count;
    uint64_t scene_generation;
    Fl_Id *selected;
    size_t selected_count, selected_capacity;
    Fl_ExportState seen_state;
    uint64_t seen_frames, seen_preview, seen_generation;
    double seen_elapsed;
    char seen_message[256];
    bool seen_init;
};

/* Rebuild the panel on the next frame. Closing the panel does not cancel the export. */
static void refresh(Fl_ExportWindow *window)
{
    if (window->dock) ca_dock_invalidate_panel(window->dock, FL_EXPORT_PANEL_ID);
}

typedef struct Fl_ExportPlan {
    bool ok;
    char detail[384];
    char path[FL_PATH_MAX];
    char height_text[32];
    uint32_t width, height, fps, crf;
    double crossfade;
    uint64_t frames;
} Fl_ExportPlan;

/* Copy an edited field and rebuild the derived size, duration and warnings. */
static void remember(char *dest, size_t capacity, Ca_TextInput *input, Fl_ExportWindow *window)
{
    snprintf(dest, capacity, "%s", ca_input_text(input));
    window->error[0] = 0;
    refresh(window);
}

static void file_changed(Ca_TextInput *input, void *data) { Fl_ExportWindow *w = data; remember(w->file_name, sizeof(w->file_name), input, w); }
static void folder_changed(Ca_TextInput *input, void *data) { Fl_ExportWindow *w = data; remember(w->folder, sizeof(w->folder), input, w); }
static void width_changed(Ca_TextInput *input, void *data) { Fl_ExportWindow *w = data; remember(w->width, sizeof(w->width), input, w); }
static void fps_changed(Ca_TextInput *input, void *data) { Fl_ExportWindow *w = data; remember(w->fps, sizeof(w->fps), input, w); }
static void crf_changed(Ca_TextInput *input, void *data) { Fl_ExportWindow *w = data; remember(w->crf, sizeof(w->crf), input, w); }
static void crossfade_changed(Ca_TextInput *input, void *data) { Fl_ExportWindow *w = data; remember(w->crossfade, sizeof(w->crossfade), input, w); }

/* Slider values are whole CRF steps or crossfade seconds shown to two places. */
static void crf_slid(Ca_Slider *slider, void *data)
{
    Fl_ExportWindow *w = data;
    int value = (int)lroundf(ca_slider_get(slider));
    if (value < 0) value = 0;
    if (value > 51) value = 51;
    snprintf(w->crf, sizeof(w->crf), "%d", value);
    w->error[0] = 0;
    refresh(w);
}

static void crossfade_slid(Ca_Slider *slider, void *data)
{
    Fl_ExportWindow *w = data;
    float value = ca_slider_get(slider);
    if (value < 0) value = 0;
    if (value > 60) value = 60;
    snprintf(w->crossfade, sizeof(w->crossfade), "%.2f", value);
    w->error[0] = 0;
    refresh(w);
}

static void scene_toggled(Ca_Checkbox *box, void *data)
{
    Fl_ExportScene *scene = data;
    scene->selected = ca_checkbox_get(box);
    scene->window->error[0] = 0;
    refresh(scene->window);
}

static void select_scenes(Fl_ExportWindow *w, bool selected)
{
    for (size_t i = 0; i < w->scene_count; ++i) w->scenes[i].selected = selected;
    w->error[0] = 0;
    refresh(w);
}
static void select_all(Ca_Button *button, void *data) { (void)button; select_scenes(data, true); }
static void select_none(Ca_Button *button, void *data) { (void)button; select_scenes(data, false); }

/* Keep scene choices across project edits. New scenes start selected. */
static void sync_scenes(Fl_ExportWindow *w)
{
    uint64_t generation = fl_project_generation(w->project);
    if (generation == w->scene_generation) return;
    size_t count = fl_project_scene_count(w->project);
    Fl_ExportScene *next = count ? calloc(count, sizeof(*next)) : NULL;
    if (count && !next) return;
    for (size_t i = 0; i < count; ++i) {
        const Fl_Scene *scene = fl_project_scene_at(w->project, i);
        bool selected = true;
        for (size_t j = 0; j < w->scene_count; ++j)
            if (w->scenes[j].id == scene->id) { selected = w->scenes[j].selected; break; }
        next[i].window = w;
        next[i].id = scene->id;
        next[i].selected = selected;
        snprintf(next[i].name, sizeof(next[i].name), "%s", scene->name && scene->name[0] ? scene->name : "Untitled");
        snprintf(next[i].box_id, sizeof(next[i].box_id), "export-scene-%llu", (unsigned long long)scene->id);
    }
    free(w->scenes);
    w->scenes = next;
    w->scene_count = count;
    w->scene_generation = generation;
}

static bool whole(const char *text, uint32_t limit, uint32_t *out)
{
    if (!text || !text[0]) return false;
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end || errno == ERANGE || value > limit) return false;
    *out = (uint32_t)value;
    return true;
}

static bool number(const char *text, double limit, double *out)
{
    if (!text || !text[0]) return false;
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (end == text || *end || errno == ERANGE || !isfinite(value) || value < 0 || value > limit) return false;
    *out = value;
    return true;
}

static bool file_name_ok(const char *name)
{
    if (!name || !name[0] || !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const char *p = name; *p; ++p)
        if (*p == '/' || *p == '\\' || *p == ':' || (unsigned char)*p < 32) return false;
    return true;
}

static bool ends_with_mp4(const char *name)
{
    size_t length = strlen(name);
    if (length < 4 || name[length - 4] != '.') return false;
    char a = name[length - 3], b = name[length - 2];
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
    return a == 'm' && b == 'p' && name[length - 1] == '4';
}

/* Frames covering one scene, matching the exporter's rounding. */
static uint64_t scene_frames(const Fl_Scene *scene, uint32_t fps)
{
    double seconds = (double)fl_scene_length(scene) / FL_TICKS_PER_SECOND;
    uint64_t frames = (uint64_t)ceil(seconds * (double)fps - 1e-9);
    return frames ? frames : 1;
}

static bool ensure_ids(Fl_ExportWindow *w, size_t count)
{
    if (count <= w->selected_capacity) return true;
    if (count > SIZE_MAX / sizeof(Fl_Id)) return false;
    Fl_Id *grown = realloc(w->selected, count * sizeof(*grown));
    if (!grown) return false;
    w->selected = grown;
    w->selected_capacity = count;
    return true;
}

static void fail(Fl_ExportPlan *plan, const char *detail)
{
    plan->ok = false;
    snprintf(plan->detail, sizeof(plan->detail), "%s", detail);
}

/* Resolve the form into the same request the exporter will validate. */
static void compose(Fl_ExportWindow *w, Fl_ExportPlan *plan)
{
    *plan = (Fl_ExportPlan){0};
    snprintf(plan->height_text, sizeof(plan->height_text), "—");
    if (!fl_ffmpeg_path()) { fail(plan, "ffmpeg was not found. Install it or set FLOW_FFMPEG."); return; }
    if (!file_name_ok(w->file_name)) { fail(plan, "The file name cannot include a folder."); return; }
    if (!w->folder[0]) { fail(plan, "Choose an export folder."); return; }
    char name[FL_PATH_MAX];
    int length = ends_with_mp4(w->file_name) ? snprintf(name, sizeof(name), "%s", w->file_name)
                                              : snprintf(name, sizeof(name), "%s.mp4", w->file_name);
    if (length < 0 || (size_t)length >= sizeof(name) || !fl_fs_join(plan->path, sizeof(plan->path), w->folder, name)) {
        fail(plan, "That export path is too long."); return;
    }
    if (!whole(w->width, 16384, &plan->width) || plan->width < 2) { fail(plan, "Width is a whole number from 2 to 16384."); return; }
    if (!whole(w->fps, 120, &plan->fps) || plan->fps < 1) { fail(plan, "Frame rate is a whole number from 1 to 120."); return; }
    if (!whole(w->crf, 51, &plan->crf)) { fail(plan, "CRF is a whole number from 0 to 51. 0 encodes as 16."); return; }
    if (!number(w->crossfade, 60, &plan->crossfade)) { fail(plan, "Crossfade is 0 to 60 seconds."); return; }
    if (!w->scene_count) { fail(plan, "This project has no scenes."); return; }
    if (!ensure_ids(w, w->scene_count)) { fail(plan, "Out of memory."); return; }
    w->selected_count = 0;
    uint64_t total = 0, shortest = UINT64_MAX;
    for (size_t i = 0; i < w->scene_count; ++i) {
        if (!w->scenes[i].selected) continue;
        const Fl_Scene *scene = fl_project_scene(w->project, w->scenes[i].id);
        uint32_t frame_w = 0, frame_h = 0;
        if (!scene || !fl_frame_dimensions(scene, plan->width, &frame_w, &frame_h)) {
            fail(plan, "A selected scene is missing or the width is not a valid size."); return;
        }
        if (w->selected_count && (frame_w != plan->width || frame_h != plan->height)) {
            fail(plan, "Selected scenes have different aspect ratios. Export them separately."); return;
        }
        plan->width = frame_w;
        plan->height = frame_h;
        snprintf(plan->height_text, sizeof(plan->height_text), "%u", frame_h);
        uint64_t frames = scene_frames(scene, plan->fps);
        if (frames > UINT64_MAX - total) { fail(plan, "The export has too many frames."); return; }
        total += frames;
        if (frames < shortest) shortest = frames;
        w->selected[w->selected_count++] = scene->id;
    }
    if (!w->selected_count) { fail(plan, "Select at least one scene."); return; }
    if ((plan->width | plan->height) & 1) {
        snprintf(plan->detail, sizeof(plan->detail), "The output size %u×%u must be even. Choose another width.", plan->width, plan->height);
        return;
    }
    uint64_t overlap = w->selected_count > 1 ? (uint64_t)llround(plan->crossfade * plan->fps) : 0;
    if (overlap >= shortest) { fail(plan, "Crossfade must be shorter than every selected scene."); return; }
    total -= overlap * (w->selected_count - 1);
    plan->frames = total;
    plan->ok = true;
    uint32_t crf = plan->crf ? plan->crf : 16;
    double fade = w->selected_count > 1 ? plan->crossfade : 0;
    double seconds = (double)total / (double)plan->fps;
    snprintf(plan->detail, sizeof(plan->detail), "%u×%u · %u fps · CRF %u · crossfade %.2f s · %llu frames · %.1f s",
        plan->width, plan->height, plan->fps, crf, fade, (unsigned long long)total, seconds);
}

static void export_clicked(Ca_Button *button, void *data)
{
    (void)button;
    Fl_ExportWindow *w = data;
    Fl_ExportPlan plan;
    compose(w, &plan);
    if (!plan.ok) { snprintf(w->error, sizeof(w->error), "%s", plan.detail); refresh(w); return; }
    if (!fl_fs_mkdir_p(w->folder)) {
        snprintf(w->error, sizeof(w->error), "Cannot create the export folder.");
        refresh(w);
        return;
    }
    char error[256] = {0};
    if (!fl_exporter_start(w->exporter, w->project, &(Fl_ExportDesc){
            .path = plan.path, .width = plan.width, .fps = plan.fps, .crf = plan.crf,
            .scenes = w->selected, .scene_count = w->selected_count, .crossfade = plan.crossfade}, error, sizeof(error)))
        snprintf(w->error, sizeof(w->error), "%s", error[0] ? error : "Export failed.");
    else w->error[0] = 0;
    refresh(w);
}

static void cancel_clicked(Ca_Button *button, void *data)
{
    (void)button;
    Fl_ExportWindow *w = data;
    fl_exporter_cancel(w->exporter);
    refresh(w);
}

static void folder_chosen(const char *const *paths, size_t count, void *data)
{
    Fl_ExportWindow *w = data;
    if (!count || !paths || !paths[0] || !paths[0][0]) return;
    snprintf(w->folder, sizeof(w->folder), "%s", paths[0]);
    w->error[0] = 0;
    refresh(w);
}

static void browse_clicked(Ca_Button *button, void *data)
{
    (void)button;
    Fl_ExportWindow *w = data;
    if (w->folder[0]) fl_fs_mkdir_p(w->folder);
    if (!fl_file_browser_show(w->browser, &(Fl_FileBrowserDesc){
            .mode = FL_FILE_BROWSER_SELECT_FOLDER, .title = "Export folder", .confirm_label = "Choose",
            .initial_path = w->folder[0] ? w->folder : NULL, .on_confirm = folder_chosen, .user_data = w})) {
        snprintf(w->error, sizeof(w->error), "The folder browser could not open.");
        refresh(w);
    }
}

static float stage_logical_w(const Fl_ExportWindow *w)
{
    if (w->layout_w > 16.f) return w->layout_w;
    if (w->stage_h > 16.f && w->stage_w > 16.f) return w->stage_w * (FL_EXPORT_STRIP_H / w->stage_h);
    return 360.f;
}

static void clear_cards(Fl_ExportWindow *w)
{
    for (int i = 0; i < w->card_count; ++i)
        if (w->cards[i].image) ca_image_destroy(w->instance, w->cards[i].image);
    memset(w->cards, 0, sizeof(w->cards));
    w->card_count = 0;
    w->last_preview_ns = 0;
    w->interval_ns = 0;
}

/* 0 when the newest frame has just arrived, 1 when the next one is due. */
static float card_travel(const Fl_ExportWindow *w)
{
    if (!w->card_count || !w->last_preview_ns || !w->interval_ns) return 1.f;
    uint64_t elapsed = fl_clock_monotonic_ns() - w->last_preview_ns;
    float travel = (float)elapsed / (float)w->interval_ns;
    if (travel < 0.f) travel = 0.f;
    if (travel > 1.f) travel = 1.f;
    return travel;
}

/* Ease through the gap between encoded frames. Still 0 and 1 at the ends, so a new card continues the last one's motion. */
static float card_motion(const Fl_ExportWindow *w)
{
    float t = card_travel(w);
    return t * t * (3.f - 2.f * t);
}

/* 1 sits on the right deck, 0 on the left. The step matches one encoded frame. */
static float card_pos(const Fl_ExportWindow *w, int index)
{
    float pos = 1.f - ((float)index + card_motion(w)) / (float)FL_EXPORT_CARDS;
    if (pos < 0.f) pos = 0.f;
    if (pos > 1.f) pos = 1.f;
    return pos;
}

/* Compress the ends into decks and let the cards crossing the middle spread out. */
static void card_pose(float stage_w, float pos, float aspect, float *x, float *y, float *w, float *h, float *opacity, float *rotation)
{
    float signed_p = pos * 2.f - 1.f;
    float compressed = copysignf(powf(fabsf(signed_p), 0.42f), signed_p);
    float margin = stage_w * 0.055f;
    if (margin < 34.f) margin = 34.f;
    if (margin > stage_w * 0.18f) margin = stage_w * 0.18f;
    float center_x = stage_w * 0.5f + compressed * (stage_w * 0.5f - margin);
    float dist = fabsf(signed_p);
    float scale = 1.f - dist * dist * 0.42f;
    float center_h = FL_EXPORT_CARD_H;
    float center_w = center_h * (aspect > 0.f ? aspect : 1.f);
    float limit = stage_w * 0.20f;
    if (limit < 56.f) limit = 56.f;
    if (center_w > limit) center_w = limit;
    *w = center_w * scale;
    *h = center_h * scale;
    *x = center_x - *w * 0.5f;
    *y = (FL_EXPORT_STRIP_H - *h) * 0.5f + (1.f - sinf(pos * 3.14159265f)) * 3.f;
    float tilt = signed_p * 10.f;
    if (dist > 0.62f) tilt *= 1.f + (dist - 0.62f) * 0.8f;
    if (tilt > 12.f) tilt = 12.f;
    if (tilt < -12.f) tilt = -12.f;
    *rotation = tilt;
    *opacity = dist > 0.94f ? 0.62f + (1.f - dist) / 0.06f * 0.38f : 1.f;
}

/* Each new encoded frame enters from the right and the trail steps left with it. */
static void present_preview(Fl_ExportWindow *w)
{
    Fl_ExportPreview preview = {0};
    if (!fl_exporter_preview(w->exporter, &preview)) {
        clear_cards(w);
        w->image_sequence = 0;
        return;
    }
    if (preview.sequence == w->image_sequence) return;
    Ca_Image *image = ca_image_create(w->instance, preview.rgba, (int)preview.width, (int)preview.height);
    if (!image) return;
    w->image_sequence = preview.sequence;
    uint64_t now = fl_clock_monotonic_ns();
    if (w->last_preview_ns && now > w->last_preview_ns) {
        uint64_t gap = now - w->last_preview_ns;
        if (gap >= 8000000ull && gap <= 1500000000ull)
            w->interval_ns = w->interval_ns ? (w->interval_ns * 2ull + gap) / 3ull : gap;
    }
    if (!w->interval_ns) w->interval_ns = 120000000ull;
    if (w->interval_ns < 16000000ull) w->interval_ns = 16000000ull;
    if (w->interval_ns > 800000000ull) w->interval_ns = 800000000ull;
    w->last_preview_ns = now;
    if (w->card_count == FL_EXPORT_CARDS) {
        if (w->cards[w->card_count - 1].image) ca_image_destroy(w->instance, w->cards[w->card_count - 1].image);
        w->card_count--;
    }
    if (w->card_count > 0)
        memmove(&w->cards[1], &w->cards[0], (size_t)w->card_count * sizeof(w->cards[0]));
    memset(&w->cards[0], 0, sizeof(w->cards[0]));
    w->cards[0].image = image;
    w->cards[0].src_w = preview.width;
    w->cards[0].src_h = preview.height;
    w->cards[0].id = ++w->next_card_id;
    w->card_count++;
}

static void format_clock(double seconds, char *out, size_t capacity)
{
    if (!(seconds >= 0) || seconds > 86400.0 * 7.0) { snprintf(out, capacity, "--:--"); return; }
    int whole = (int)seconds;
    int minutes = whole / 60, secs = whole % 60, hours = minutes / 60;
    minutes %= 60;
    if (hours) snprintf(out, capacity, "%d:%02d:%02d", hours, minutes, secs);
    else snprintf(out, capacity, "%d:%02d", minutes, secs);
}

static int export_percent(const Fl_ExportStatus *status)
{
    if (status->state == FL_EXPORT_DONE) return 100;
    if (!status->frames_total) return 0;
    int percent = (int)lround(100.0 * (double)status->frames_done / (double)status->frames_total);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return percent;
}

static void format_headline(const Fl_ExportStatus *status, char *out, size_t capacity)
{
    if (status->state == FL_EXPORT_IDLE) { snprintf(out, capacity, "Ready"); return; }
    const char *phase = status->message[0] ? status->message : "Export";
    if (status->frames_total) snprintf(out, capacity, "%s · %d%%", phase, export_percent(status));
    else snprintf(out, capacity, "%s", phase);
}

static void format_detail(const Fl_ExportStatus *status, char *out, size_t capacity)
{
    out[0] = 0;
    if (status->state == FL_EXPORT_IDLE || !status->frames_total) return;
    char elapsed[16], remain[16];
    format_clock(status->elapsed_seconds, elapsed, sizeof(elapsed));
    if (status->state == FL_EXPORT_RUNNING && status->frames_done && status->frames_done < status->frames_total && status->elapsed_seconds > 0.05) {
        double left = (double)(status->frames_total - status->frames_done) * status->elapsed_seconds / (double)status->frames_done;
        format_clock(left, remain, sizeof(remain));
        snprintf(out, capacity, "%llu / %llu frames · %s elapsed · %s left",
            (unsigned long long)status->frames_done, (unsigned long long)status->frames_total, elapsed, remain);
        return;
    }
    snprintf(out, capacity, "%llu / %llu frames · %s",
        (unsigned long long)status->frames_done, (unsigned long long)status->frames_total, elapsed);
}

static uint32_t bar_color(Fl_ExportState state)
{
    if (state == FL_EXPORT_DONE) return ca_color(0.298f, 0.765f, 0.541f, 1.f);
    if (state == FL_EXPORT_FAILED) return ca_color(0.941f, 0.439f, 0.478f, 1.f);
    if (state == FL_EXPORT_CANCELLED) return ca_color(0.910f, 0.690f, 0.290f, 1.f);
    return ca_color(0.416f, 0.651f, 1.f, 1.f);
}

static const char *head_style(Fl_ExportState state)
{
    if (state == FL_EXPORT_DONE) return FL_CLASS_EXPORT_HEAD_OK;
    if (state == FL_EXPORT_FAILED || state == FL_EXPORT_CANCELLED) return FL_CLASS_EXPORT_HEAD_WARN;
    return FL_CLASS_EXPORT_HEAD;
}

static void emit_cards(Fl_ExportWindow *w)
{
    float stage_w = stage_logical_w(w);
    for (int i = w->card_count - 1; i >= 0; --i) {
        Fl_ExportCard *card = &w->cards[i];
        if (!card->image || !card->src_w || !card->src_h) continue;
        float pos = card_pos(w, i);
        float x = 0, y = 0, width = 0, height = 0, opacity = 1.f, rotation = 0.f;
        card_pose(stage_w, pos, (float)card->src_w / (float)card->src_h, &x, &y, &width, &height, &opacity, &rotation);
        char id[40], image_id[48], fade[32];
        snprintf(id, sizeof(id), "export-frame-%llu", (unsigned long long)card->id);
        snprintf(image_id, sizeof(image_id), "export-frame-px-%llu", (unsigned long long)card->id);
        snprintf(fade, sizeof(fade), "opacity: %.3f", opacity);
        float focus = 1.f - fminf(fabsf(pos - 0.5f) * 4.f, 1.f);
        ca_div_begin(&(Ca_DivDesc){
            .id = id, .style = FL_CLASS_EXPORT_FRAME, .inline_style = fade,
            .position = CA_POSITION_ABSOLUTE, .pos_x = x, .pos_y = y,
            .width = width, .height = height,
            .z_index = 12 + (int)(pos * 24.f + focus * 48.f),
            .rotation = rotation, .clip_content = true, .corner_radius = 9.f, .no_hover = true,
            .shadow_offset_y = 3.f, .shadow_blur = 12.f, .shadow_color = ca_color(0.f, 0.f, 0.f, 0.38f),
        });
        ca_image(&(Ca_ImageDesc){.image = card->image, .id = image_id, .width = width, .height = height});
        ca_div_end();
    }
}

static void begin_row(const char *label)
{
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_EXPORT_ROW});
    ca_text(&(Ca_TextDesc){.text = label, .style = FL_CLASS_EXPORT_LABEL});
}

static float slider_value(const char *text, float limit)
{
    double value = 0;
    if (!number(text, limit, &value)) return 0;
    return (float)value;
}

/* Rebuild the panel from the form and the exporter's latest frame. */
void fl_export_panel_build(Ca_Dock *dock, Ca_Window *window, void *data)
{
    (void)window;
    Fl_ExportWindow *w = data;
    w->dock = dock;
    sync_scenes(w);
    present_preview(w);
    Fl_ExportStatus status = fl_exporter_status(w->exporter);
    bool running = status.state == FL_EXPORT_RUNNING;
    Fl_ExportPlan plan;
    compose(w, &plan);
    float progress = 0;
    if (status.frames_total) progress = (float)((double)status.frames_done / (double)status.frames_total);
    if (status.state == FL_EXPORT_DONE) progress = 1;
    char headline[192], detail[192];
    format_headline(&status, headline, sizeof(headline));
    format_detail(&status, detail, sizeof(detail));
    const char *path = status.state != FL_EXPORT_IDLE && status.path[0] ? status.path : plan.path;

    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_EXPORT});
    w->stage = ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = "export-stage", .style = FL_CLASS_EXPORT_STAGE});
    emit_cards(w);
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .id = "export-track", .style = FL_CLASS_EXPORT_TRACK});
    ca_progress(&(Ca_ProgressDesc){.value = progress, .id = "export-progress", .style = FL_CLASS_EXPORT_PROGRESS,
        .height = 8, .bar_color = bar_color(status.state)});
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = "export-body", .style = FL_CLASS_EXPORT_BODY});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_EXPORT_STATUS});
    ca_text(&(Ca_TextDesc){.text = headline, .id = "export-status", .style = head_style(status.state)});
    if (detail[0]) ca_text(&(Ca_TextDesc){.text = detail, .id = "export-detail", .style = FL_CLASS_EXPORT_NOTE});
    if (path && path[0]) ca_text(&(Ca_TextDesc){.text = path, .id = "export-path", .style = FL_CLASS_EXPORT_NOTE, .wrap = true});
    if (running) ca_text(&(Ca_TextDesc){.text = "You can close this panel. The export keeps running.", .id = "export-keep-running", .style = FL_CLASS_EXPORT_NOTE});
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = "export-form", .style = FL_CLASS_EXPORT_FORM});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_EXPORT_CARD});
    ca_text(&(Ca_TextDesc){.text = "Output", .style = FL_CLASS_HEADING});
    begin_row("File");
    ca_input(&(Ca_InputDesc){.text = w->file_name, .id = "export-file", .style = FL_CLASS_PROPERTY_INPUT,
        .placeholder = "export.mp4", .on_change = file_changed, .change_data = w, .disabled = running});
    ca_div_end();
    begin_row("Folder");
    ca_input(&(Ca_InputDesc){.text = w->folder, .id = "export-folder", .style = FL_CLASS_PROPERTY_INPUT,
        .on_change = folder_changed, .change_data = w, .disabled = running});
    fl_button(&(Fl_ButtonDesc){.text = "Browse", .id = "export-browse", .on_click = browse_clicked, .click_data = w, .disabled = running,
        .tooltip = "Choose the export folder"});
    ca_div_end();
    begin_row("Width");
    ca_input(&(Ca_InputDesc){.text = w->width, .id = "export-width", .style = FL_CLASS_EXPORT_NUMBER, .input_mode = CA_INPUT_UINT,
        .on_change = width_changed, .change_data = w, .disabled = running});
    ca_text(&(Ca_TextDesc){.text = "×", .style = FL_CLASS_MUTED});
    ca_text(&(Ca_TextDesc){.text = plan.height_text, .style = FL_CLASS_LABEL});
    ca_div_end();
    begin_row("Frame rate");
    ca_input(&(Ca_InputDesc){.text = w->fps, .id = "export-fps", .style = FL_CLASS_EXPORT_NUMBER, .input_mode = CA_INPUT_UINT,
        .on_change = fps_changed, .change_data = w, .disabled = running});
    ca_div_end();
    begin_row("CRF");
    ca_slider(&(Ca_SliderDesc){.min = 0, .max = 51, .value = slider_value(w->crf, 51), .id = "export-crf-slider",
        .style = FL_CLASS_EXPORT_SLIDER, .on_change = crf_slid, .change_data = w, .disabled = running});
    ca_input(&(Ca_InputDesc){.text = w->crf, .id = "export-crf", .style = FL_CLASS_EXPORT_NUMBER, .input_mode = CA_INPUT_UINT,
        .on_change = crf_changed, .change_data = w, .disabled = running});
    ca_div_end();
    begin_row("Crossfade");
    ca_slider(&(Ca_SliderDesc){.min = 0, .max = 60, .value = slider_value(w->crossfade, 60), .id = "export-crossfade-slider",
        .style = FL_CLASS_EXPORT_SLIDER, .on_change = crossfade_slid, .change_data = w, .disabled = running});
    ca_input(&(Ca_InputDesc){.text = w->crossfade, .id = "export-crossfade", .style = FL_CLASS_EXPORT_NUMBER, .input_mode = CA_INPUT_FLOAT,
        .on_change = crossfade_changed, .change_data = w, .disabled = running});
    ca_div_end();
    ca_text(&(Ca_TextDesc){.text = "Height follows the scene. Both dimensions must be even. CRF 0 encodes as 16.", .style = FL_CLASS_EXPORT_NOTE, .wrap = true});
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .id = "export-scenes", .style = FL_CLASS_EXPORT_CARD});
    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_EXPORT_ROW});
    ca_text(&(Ca_TextDesc){.text = "Scenes", .style = FL_CLASS_HEADING});
    ca_div_begin(&(Ca_DivDesc){.style = FL_CLASS_TOOLBAR_SPACER});
    ca_div_end();
    fl_button(&(Fl_ButtonDesc){.text = "All", .id = "export-all", .on_click = select_all, .click_data = w, .disabled = running});
    fl_button(&(Fl_ButtonDesc){.text = "None", .id = "export-none", .on_click = select_none, .click_data = w, .disabled = running});
    ca_div_end();
    if (!w->scene_count) ca_text(&(Ca_TextDesc){.text = "This project has no scenes.", .style = FL_CLASS_MUTED});
    for (size_t i = 0; i < w->scene_count; ++i)
        ca_checkbox(&(Ca_CheckboxDesc){.text = w->scenes[i].name, .checked = w->scenes[i].selected, .id = w->scenes[i].box_id,
            .on_change = scene_toggled, .change_data = &w->scenes[i], .disabled = running});
    ca_div_end();
    ca_div_end();

    if (w->error[0] || !plan.ok || !running) {
        ca_div_begin(&(Ca_DivDesc){.direction = CA_VERTICAL, .style = FL_CLASS_EXPORT_STATUS});
        ca_text(&(Ca_TextDesc){.text = w->error[0] ? w->error : plan.detail, .id = "export-summary", .wrap = true,
            .style = w->error[0] || !plan.ok ? FL_CLASS_EXPORT_NOTE_WARN : FL_CLASS_EXPORT_NOTE});
        ca_div_end();
    }
    ca_div_end();

    ca_div_begin(&(Ca_DivDesc){.direction = CA_HORIZONTAL, .style = FL_CLASS_EXPORT_FOOTER});
    fl_button(&(Fl_ButtonDesc){.text = "Cancel", .id = "export-cancel", .on_click = cancel_clicked, .click_data = w,
        .disabled = !running, .destructive = true, .tooltip = "Stop the export and keep the previous file"});
    ca_div_begin(&(Ca_DivDesc){.style = FL_CLASS_TOOLBAR_SPACER});
    ca_div_end();
    fl_button(&(Fl_ButtonDesc){.text = running ? "Exporting…" : "Export", .id = "export-go", .on_click = export_clicked, .click_data = w,
        .disabled = running || !plan.ok, .tooltip = running ? "An export is already running" : "Write the video"});
    ca_div_end();
    ca_div_end();
}

void fl_export_panel_teardown(void *data)
{
    Fl_ExportWindow *w = data;
    if (!w) return;
    w->dock = NULL;
    w->stage = NULL;
}

/* Follow the exporter while the panel is showing. Elapsed time advances once a second. */
static void watch(void *data)
{
    Fl_ExportWindow *w = data;
    if (!w->dock || !w->exporter) return;
    bool resized = false;
    if (w->stage) {
        float x = 0, y = 0, width = 0, height = 0;
        ca_div_content_screen_rect(w->stage, &x, &y, &width, &height);
        if (width > 16.f && height > 16.f && (fabsf(width - w->stage_w) > 4.f || fabsf(height - w->stage_h) > 4.f)) {
            w->stage_w = width;
            w->stage_h = height;
            float logical = width * (FL_EXPORT_STRIP_H / height);
            if (fabsf(logical - w->layout_w) > 4.f) {
                w->layout_w = logical;
                resized = true;
            }
        }
    }
    bool sliding = w->card_count && card_travel(w) < 0.999f;
    Fl_ExportStatus status = fl_exporter_status(w->exporter);
    if (sliding) ca_instance_request_frame_after(w->instance, 0);
    else if (status.state == FL_EXPORT_RUNNING) ca_instance_request_frame_after(w->instance, 0.5);
    Fl_ExportPreview preview = {0};
    fl_exporter_preview(w->exporter, &preview);
    uint64_t generation = fl_project_generation(w->project);
    bool changed = sliding || resized || !w->seen_init || status.state != w->seen_state || status.frames_done != w->seen_frames ||
        (int)status.elapsed_seconds != (int)w->seen_elapsed || preview.sequence != w->seen_preview ||
        generation != w->seen_generation || strcmp(status.message, w->seen_message) != 0;
    w->seen_init = true;
    w->seen_state = status.state;
    w->seen_frames = status.frames_done;
    w->seen_elapsed = status.elapsed_seconds;
    w->seen_preview = preview.sequence;
    w->seen_generation = generation;
    snprintf(w->seen_message, sizeof(w->seen_message), "%s", status.message);
    if (changed) refresh(w);
}

Fl_ExportWindow *fl_export_window_create(Ca_Instance *instance, Fl_Project *project, Fl_Exporter *exporter)
{
    if (!instance || !project || !exporter || !fl_project_directory(project)) return NULL;
    Fl_ExportWindow *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->instance = instance;
    w->project = project;
    w->exporter = exporter;
    w->scene_generation = UINT64_MAX;
    snprintf(w->file_name, sizeof(w->file_name), "export.mp4");
    snprintf(w->width, sizeof(w->width), "1920");
    snprintf(w->fps, sizeof(w->fps), "60");
    snprintf(w->crf, sizeof(w->crf), "16");
    snprintf(w->crossfade, sizeof(w->crossfade), "0");
    if (!fl_fs_join(w->folder, sizeof(w->folder), fl_project_directory(project), "exports")) { free(w); return NULL; }
    w->watch = ca_frame_effect(instance, watch, w);
    w->browser = fl_file_browser_create(instance);
    if (!w->watch || !w->browser) { fl_export_window_destroy(w); return NULL; }
    return w;
}

void fl_export_window_destroy(Fl_ExportWindow *w)
{
    if (!w) return;
    ca_effect_destroy(w->watch);
    w->watch = NULL;
    fl_file_browser_destroy(w->browser);
    w->browser = NULL;
    w->dock = NULL;
    w->stage = NULL;
    clear_cards(w);
    free(w->scenes);
    free(w->selected);
    free(w);
}

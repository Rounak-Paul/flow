#include "fl_manuscript.h"
#include "fl_timeline_internal.h"
#include "doc/fl_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const SPEAKERS[] = {
    "Ryan", "Aiden", "Vivian", "Serena", "Uncle_Fu", "Dylan", "Eric", "Ono_Anna", "Sohee"
};
static const char *const LANGUAGES[] = {
    "Auto", "Chinese", "English", "Japanese", "Korean", "German", "French", "Russian", "Portuguese", "Spanish", "Italian"
};

const char *const *fl_manuscript_speakers(size_t *count)
{
    if (count) *count = sizeof(SPEAKERS) / sizeof(*SPEAKERS);
    return SPEAKERS;
}
const char *const *fl_manuscript_languages(size_t *count)
{
    if (count) *count = sizeof(LANGUAGES) / sizeof(*LANGUAGES);
    return LANGUAGES;
}

/* A CustomVoice speaker id: a letter, then letters, digits and underscores. */
bool fl_manuscript_speaker_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (!n || n > FL_VOICE_NAME_MAX || !fl_json_utf8_valid(name, n)) return false;
    if (!((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'))) return false;
    for (size_t i = 1; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}
bool fl_manuscript_language_ok(const char *name)
{
    if (!name) return false;
    for (size_t i = 0; i < sizeof(LANGUAGES) / sizeof(*LANGUAGES); ++i) if (!strcmp(name, LANGUAGES[i])) return true;
    return false;
}

size_t fl_manuscript_words(const char *text)
{
    size_t words = 0;
    bool in = false;
    if (!text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        bool space = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v';
        if (space) in = false;
        else if (!in) { in = true; ++words; }
    }
    return words;
}
double fl_manuscript_seconds(const char *text)
{
    return (double)fl_manuscript_words(text) * 60.0 / (double)FL_NARRATION_WPM;
}
void fl_manuscript_format(double seconds, char *out, size_t capacity)
{
    if (!out || !capacity) return;
    if (seconds < 0) seconds = 0;
    if (seconds < 59.5) snprintf(out, capacity, "0:%02d", (int)(seconds + 0.5));
    else snprintf(out, capacity, "%.1f min", seconds / 60.0);
}

const Fl_Phase *fl_scene_phase(const Fl_Scene *scene, Fl_Id phase)
{
    if (!scene || !phase) return NULL;
    for (size_t i = 0; i < scene->phase_count; ++i) if (scene->phases[i].id == phase) return &scene->phases[i];
    return NULL;
}

static char *copy_text(const char *text)
{
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);
    if (copy) memcpy(copy, text, n);
    return copy;
}
static bool bounded_text(const char *text, size_t limit, bool allow_empty)
{
    size_t n = text ? strlen(text) : 0;
    return text && (allow_empty || n) && n <= limit && fl_json_utf8_valid(text, n);
}
static size_t phase_index(const Fl_Scene *s, Fl_Id phase)
{
    for (size_t i = 0; i < s->phase_count; ++i) if (s->phases[i].id == phase) return i;
    return SIZE_MAX;
}
static size_t beat_index(const Fl_Scene *s, Fl_Id beat)
{
    for (size_t i = 0; i < s->beat_count; ++i) if (s->beats[i].id == beat) return i;
    return SIZE_MAX;
}
static bool clip_exists(const Fl_Scene *s, Fl_Id clip)
{
    for (size_t i = 0; i < s->clip_count; ++i) if (s->clips[i].id == clip) return true;
    return false;
}
/* Ticks for a spoken line: at least one second when there are words, otherwise 0. */
static uint64_t reading_ticks(const char *text)
{
    if (!fl_manuscript_words(text)) return 0;
    double seconds = fl_manuscript_seconds(text);
    if (seconds < 1.0) seconds = 1.0;
    double ticks = seconds * (double)FL_TICKS_PER_SECOND + 0.5;
    if (ticks > (double)(UINT64_C(1) << 62)) return UINT64_C(1) << 62;
    return (uint64_t)ticks;
}
static bool reserve_phases(Fl_Scene *s, size_t extra)
{
    if (s->phase_count > SIZE_MAX / sizeof(Fl_Phase) - extra) return false;
    Fl_Phase *phases = realloc((void *)s->phases, (s->phase_count + extra) * sizeof(*phases));
    if (!phases) return false;
    s->phases = phases;
    return true;
}

/* The first phase of a fresh scene narrates its only beat. Later phases append a beat. */
static bool bind_beat(const Fl_Project *project, Fl_Scene *s, const char *title, const char *text, Fl_Id requested, uint64_t *next_id, Fl_Id *beat)
{
    if (requested) {
        if (beat_index(s, requested) == SIZE_MAX) return false;
        *beat = requested;
        return true;
    }
    uint64_t spoken = reading_ticks(text);
    if (!s->phase_count && s->beat_count == 1) {
        const Fl_Beat *current = &s->beats[0];
        const char *name = !strcmp(current->name, "Beat 1") ? title : current->name;
        uint64_t duration = spoken ? spoken : current->duration;
        if (!fl_timeline_apply(project, s, &(Fl_Op){.kind = FL_OP_BEAT_UPDATE, .beat = current->id, .name = name,
                .beat_duration = duration, .auto_advance = current->auto_advance}, next_id, NULL)) return false;
        *beat = current->id;
        return true;
    }
    uint64_t duration = spoken ? spoken : 5u * FL_TICKS_PER_SECOND;
    return fl_timeline_apply(project, s, &(Fl_Op){.kind = FL_OP_BEAT_ADD, .name = title, .position = s->beat_count,
            .beat_duration = duration, .auto_advance = false}, next_id, beat) && *beat;
}

static bool phase_add(const Fl_Project *project, Fl_Scene *s, const Fl_Op *op, uint64_t *next_id, Fl_Id *created)
{
    const char *text = op->source ? op->source : "";
    bool adopt = !op->beat && !s->phase_count && s->beat_count == 1;
    size_t ids = 1;
    if (!op->beat && !adopt) ids += s->beat_count ? 1 : 2;
    if (!op->name || s->phase_count >= FL_PHASE_MAX || op->position > s->phase_count || *next_id > UINT64_MAX - ids ||
        !bounded_text(op->name, FL_PHASE_TITLE_MAX, false) || !bounded_text(text, FL_PHASE_TEXT_MAX, true)) return false;
    char *title = copy_text(op->name), *body = copy_text(text);
    if (!title || !body || !reserve_phases(s, 1)) { free(title); free(body); return false; }
    Fl_Id beat = 0;
    if (!bind_beat(project, s, title, body, op->beat, next_id, &beat)) { free(title); free(body); return false; }
    Fl_Phase *phases = (Fl_Phase *)s->phases;
    memmove(phases + op->position + 1, phases + op->position, (s->phase_count - op->position) * sizeof(*phases));
    Fl_Id id = (*next_id)++;
    phases[op->position] = (Fl_Phase){id, beat, 0, title, body};
    ++s->phase_count;
    if (created) *created = id;
    return true;
}

static bool phase_update(Fl_Scene *s, const Fl_Op *op)
{
    size_t i = phase_index(s, op->phase);
    const char *text = op->source ? op->source : "";
    if (i == SIZE_MAX || !bounded_text(op->name, FL_PHASE_TITLE_MAX, false) || !bounded_text(text, FL_PHASE_TEXT_MAX, true)) return false;
    if (op->beat && beat_index(s, op->beat) == SIZE_MAX) return false;
    if (op->clip && !clip_exists(s, op->clip)) return false;
    char *title = copy_text(op->name), *body = copy_text(text);
    if (!title || !body) { free(title); free(body); return false; }
    Fl_Phase *phase = (Fl_Phase *)&s->phases[i];
    free((void *)phase->title); free((void *)phase->text);
    phase->title = title; phase->text = body; phase->beat = op->beat; phase->clip = op->clip;
    return true;
}

static bool phase_remove(Fl_Scene *s, const Fl_Op *op)
{
    size_t i = phase_index(s, op->phase);
    if (i == SIZE_MAX) return false;
    Fl_Phase *phases = (Fl_Phase *)s->phases;
    free((void *)phases[i].title); free((void *)phases[i].text);
    memmove(phases + i, phases + i + 1, (s->phase_count - i - 1) * sizeof(*phases));
    --s->phase_count;
    return true;
}

static bool phase_move(Fl_Scene *s, const Fl_Op *op)
{
    size_t from = phase_index(s, op->phase);
    if (from == SIZE_MAX || op->position >= s->phase_count) return false;
    Fl_Phase *phases = (Fl_Phase *)s->phases, moved = phases[from];
    if (from < op->position) memmove(phases + from, phases + from + 1, (op->position - from) * sizeof(*phases));
    else memmove(phases + op->position + 1, phases + op->position, (from - op->position) * sizeof(*phases));
    phases[op->position] = moved;
    return true;
}

static bool voice_set(const Fl_Project *project, Fl_Scene *s, const Fl_Op *op)
{
    if (!fl_manuscript_speaker_ok(op->name) || !fl_manuscript_language_ok(op->language)) return false;
    if (op->source && !bounded_text(op->source, FL_VOICE_TRANSCRIPT_MAX, true)) return false;
    if (op->asset) {
        const Fl_Asset *asset = fl_project_asset(project, op->asset);
        if (!asset || asset->kind != FL_ASSET_AUDIO) return false;
    }
    char *voice = copy_text(op->name), *language = copy_text(op->language);
    char *transcript = op->source && op->source[0] ? copy_text(op->source) : NULL;
    if (!voice || !language || (op->source && op->source[0] && !transcript)) { free(voice); free(language); free(transcript); return false; }
    free((void *)s->voice); free((void *)s->language); free((void *)s->voice_transcript);
    s->voice = voice; s->language = language; s->voice_transcript = transcript; s->voice_reference = op->asset;
    return true;
}

bool fl_manuscript_apply(const Fl_Project *project, Fl_Scene *s, const Fl_Op *op, uint64_t *next_id, Fl_Id *created)
{
    if (!project || !s || !op || !next_id) return false;
    switch (op->kind) {
    case FL_OP_PHASE_ADD: return phase_add(project, s, op, next_id, created);
    case FL_OP_PHASE_UPDATE: return phase_update(s, op);
    case FL_OP_PHASE_REMOVE: return phase_remove(s, op);
    case FL_OP_PHASE_MOVE: return phase_move(s, op);
    case FL_OP_VOICE_SET: return voice_set(project, s, op);
    default: return false;
    }
}

void fl_manuscript_detach_beat(Fl_Scene *s, Fl_Id beat)
{
    if (!s || !beat) return;
    for (size_t i = 0; i < s->phase_count; ++i) if (s->phases[i].beat == beat) ((Fl_Phase *)s->phases)[i].beat = 0;
}
void fl_manuscript_detach_clip(Fl_Scene *s, Fl_Id clip)
{
    if (!s || !clip) return;
    for (size_t i = 0; i < s->phase_count; ++i) if (s->phases[i].clip == clip) ((Fl_Phase *)s->phases)[i].clip = 0;
}

void fl_manuscript_free(Fl_Scene *s)
{
    if (!s) return;
    for (size_t i = 0; i < s->phase_count; ++i) { free((void *)s->phases[i].title); free((void *)s->phases[i].text); }
    free((void *)s->phases); free((void *)s->voice); free((void *)s->language); free((void *)s->voice_transcript);
    s->phases = NULL; s->voice = NULL; s->language = NULL; s->voice_transcript = NULL;
    s->phase_count = 0; s->voice_reference = 0;
}

bool fl_manuscript_valid(const Fl_Project *p, const Fl_Scene *s, uint64_t next_id)
{
    if (!s) return false;
    bool empty = !s->phase_count && !s->voice && !s->language && !s->voice_reference && !s->voice_transcript;
    if (empty) return true;
    if (!fl_manuscript_speaker_ok(s->voice) || !fl_manuscript_language_ok(s->language)) return false;
    if (s->voice_transcript && !bounded_text(s->voice_transcript, FL_VOICE_TRANSCRIPT_MAX, true)) return false;
    if (s->voice_reference) {
        const Fl_Asset *asset = fl_project_asset(p, s->voice_reference);
        if (!asset || asset->kind != FL_ASSET_AUDIO) return false;
    }
    if (s->phase_count > FL_PHASE_MAX) return false;
    for (size_t i = 0; i < s->phase_count; ++i) {
        const Fl_Phase *phase = &s->phases[i];
        if (!phase->id || phase->id >= next_id || !bounded_text(phase->title, FL_PHASE_TITLE_MAX, false) ||
            !bounded_text(phase->text, FL_PHASE_TEXT_MAX, true)) return false;
        if (phase->beat && beat_index(s, phase->beat) == SIZE_MAX) return false;
        if (phase->clip && !clip_exists(s, phase->clip)) return false;
    }
    return true;
}

size_t fl_manuscript_id_count(const Fl_Scene *s) { return s ? s->phase_count : 0; }
void fl_manuscript_ids(const Fl_Scene *s, Fl_Id *ids)
{
    for (size_t i = 0; s && i < s->phase_count; ++i) ids[i] = s->phases[i].id;
}
size_t fl_manuscript_asset_uses(const Fl_Scene *s, Fl_Id asset)
{
    return s && asset && s->voice_reference == asset ? 1 : 0;
}

void fl_manuscript_save(const Fl_Scene *s, Fl_Binary *b)
{
    fl_binary_string(b, s->voice ? s->voice : "Ryan");
    fl_binary_string(b, s->language ? s->language : "English");
    fl_binary_u64(b, s->voice_reference);
    fl_binary_string(b, s->voice_transcript ? s->voice_transcript : "");
    fl_binary_u64(b, s->phase_count);
    for (size_t i = 0; i < s->phase_count; ++i) {
        const Fl_Phase *phase = &s->phases[i];
        fl_binary_u64(b, phase->id); fl_binary_u64(b, phase->beat); fl_binary_u64(b, phase->clip);
        fl_binary_string(b, phase->title); fl_binary_string(b, phase->text);
    }
}

bool fl_manuscript_load(Fl_Scene *s, Fl_Binary *b)
{
    s->voice = fl_binary_get_string(b);
    s->language = fl_binary_get_string(b);
    s->voice_reference = fl_binary_get_u64(b);
    s->voice_transcript = fl_binary_get_string(b);
    if (s->voice_transcript && !s->voice_transcript[0]) { free((void *)s->voice_transcript); s->voice_transcript = NULL; }
    size_t count = 0;
    if (!b->failed) {
        uint64_t n = fl_binary_get_u64(b);
        if (b->failed || n > FL_PHASE_MAX) b->failed = true;
        else count = (size_t)n;
    }
    Fl_Phase *phases = count ? calloc(count, sizeof(*phases)) : NULL;
    if (count && !phases) return false;
    s->phases = phases;
    for (size_t i = 0; !b->failed && i < count; ++i, ++s->phase_count) {
        Fl_Phase *phase = &phases[i];
        phase->id = fl_binary_get_u64(b); phase->beat = fl_binary_get_u64(b); phase->clip = fl_binary_get_u64(b);
        phase->title = fl_binary_get_string(b); phase->text = fl_binary_get_string(b);
    }
    return !b->failed;
}

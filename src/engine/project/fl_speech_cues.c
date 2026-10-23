#include "fl_speech_cues.h"
#include "doc/fl_json.h"
#include <stdlib.h>
#include <string.h>

/* Duplicate a bounded cue label into private ownership. */
static char *copy_text(const char *text)
{
    size_t size = strlen(text) + 1; char *copy = malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}

void fl_speech_cues_free(Fl_SpeechCue *cues, size_t count)
{
    for (size_t i = 0; i < count; ++i) free((void *)cues[i].text);
    free(cues);
}

/* Read the next normalized byte without altering UTF-8 multi-byte sequences. */
static unsigned char normalized(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    while (*p && *p < 128 && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) ++p;
    unsigned char value = *p;
    if (value) ++p;
    *cursor = (const char *)p;
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + 'a' - 'A') : value;
}

bool fl_speech_cue_equal(const Fl_SpeechCue *a, const Fl_SpeechCue *b)
{
    if (a->kind != b->kind || !a->text || !b->text) return false;
    const char *left = a->text, *right = b->text;
    unsigned char x, y;
    do {x = normalized(&left); y = normalized(&right); if (x != y) return false;} while (x);
    return true;
}

const Fl_SpeechCue *fl_scene_speech_cue(const Fl_Scene *scene, Fl_Id id, const Fl_Phase **phase)
{
    for (size_t i = 0; scene && id && i < scene->phase_count; ++i)
        for (size_t j = 0; j < scene->phases[i].cue_count; ++j) if (scene->phases[i].cues[j].id == id) {
            if (phase) *phase = &scene->phases[i];
            return &scene->phases[i].cues[j];
        }
    return NULL;
}

bool fl_speech_cues_apply(const Fl_Project *project, Fl_Scene *scene, const Fl_Op *op, uint64_t *next_id)
{
    Fl_Phase *phase = NULL;
    for (size_t i = 0; i < scene->phase_count; ++i) if (scene->phases[i].id == op->phase) phase = (Fl_Phase *)&scene->phases[i];
    if (!phase || (op->cue_count && !op->cues) || op->cue_count > FL_SPEECH_CUE_MAX ||
        phase->cue_count > FL_SPEECH_CUE_MAX || *next_id > UINT64_MAX - op->cue_count) return false;
    bool asset_matches = false;
    for (size_t i = 0; i < scene->clip_count; ++i) if (scene->clips[i].id == phase->clip && scene->clips[i].asset == op->asset &&
        scene->clips[i].start.kind != FL_ANCHOR_SPEECH) asset_matches = true;
    const Fl_Asset *asset = fl_project_asset(project,op->asset);
    if (!asset_matches || !asset || asset->kind != FL_ASSET_AUDIO) return false;
    size_t capacity = phase->cue_count + op->cue_count, count = op->cue_count;
    Fl_SpeechCue *cues = capacity ? calloc(capacity, sizeof(*cues)) : NULL;
    bool *used = phase->cue_count ? calloc(phase->cue_count, sizeof(*used)) : NULL;
    if ((capacity && !cues) || (phase->cue_count && !used)) {free(cues); free(used); return false;}
    bool ok = true;
    for (size_t i = 0; i < op->cue_count; ++i) {
        cues[i] = op->cues[i]; cues[i].id = 0; cues[i].retired = false;
        cues[i].text = cues[i].text ? copy_text(cues[i].text) : NULL;
        if (!cues[i].text || !cues[i].text[0] || strlen(cues[i].text) > 16384 ||
            !fl_json_utf8_valid(cues[i].text, strlen(cues[i].text)) || cues[i].start < 0 || cues[i].end < cues[i].start ||
            ((uint64_t)cues[i].end > asset->duration || (cues[i].resolved && cues[i].end <= cues[i].start)) ||
            (cues[i].kind != FL_CUE_WORD && cues[i].kind != FL_CUE_SENTENCE)) {ok = false; break;}
    }
    /* Match each cue kind independently; exact prefix/suffix retain repeated words during ordinary insertions. */
    for (int kind = FL_CUE_WORD; ok && kind <= FL_CUE_SENTENCE; ++kind) {
        size_t *old = phase->cue_count ? malloc(phase->cue_count * sizeof(*old)) : NULL;
        size_t *fresh = count ? malloc(count * sizeof(*fresh)) : NULL;
        if ((phase->cue_count && !old) || (count && !fresh)) {free(old); free(fresh); ok = false; break;}
        size_t n = 0, m = 0;
        for (size_t i = 0; i < phase->cue_count; ++i) if (!phase->cues[i].retired && (int)phase->cues[i].kind == kind) old[n++] = i;
        for (size_t i = 0; i < count; ++i) if ((int)cues[i].kind == kind) fresh[m++] = i;
        size_t a = 0, b = 0;
        while (a < n && b < m && fl_speech_cue_equal(&phase->cues[old[a]], &cues[fresh[b]])) {
            cues[fresh[b]].id = phase->cues[old[a]].id; used[old[a]] = true; ++a; ++b;
        }
        while (a < n && b < m && fl_speech_cue_equal(&phase->cues[old[n - 1]], &cues[fresh[m - 1]])) {
            cues[fresh[m - 1]].id = phase->cues[old[n - 1]].id; used[old[n - 1]] = true; --n; --m;
        }
        /* Only unique remaining matches preserve identity; ambiguous repetitions require explicit repair. */
        size_t last = a;
        for (size_t j = b; j < m; ++j) {
            size_t match = SIZE_MAX, occurrences = 0, target_occurrences = 0;
            for (size_t i = last; i < n; ++i) if (fl_speech_cue_equal(&phase->cues[old[i]], &cues[fresh[j]])) {match = i; ++occurrences;}
            for (size_t k = b; k < m; ++k) target_occurrences += fl_speech_cue_equal(&cues[fresh[k]], &cues[fresh[j]]);
            if (occurrences == 1 && target_occurrences == 1) {
                cues[fresh[j]].id = phase->cues[old[match]].id; used[old[match]] = true; last = match + 1;
            }
        }
        /* A changed repetition count cannot identify which occurrence survived an edit. */
        for (size_t j = 0; j < count; ++j) if ((int)cues[j].kind == kind && cues[j].id) {
            size_t before = 0, after = 0, matched = SIZE_MAX;
            for (size_t i = 0; i < phase->cue_count; ++i) if (!phase->cues[i].retired && fl_speech_cue_equal(&phase->cues[i],&cues[j])) {
                ++before; if (phase->cues[i].id == cues[j].id) matched = i;
            }
            for (size_t i = 0; i < count; ++i) after += fl_speech_cue_equal(&cues[i],&cues[j]);
            if (before != after && (before > 1 || after > 1)) {if (matched != SIZE_MAX) used[matched] = false; cues[j].id = 0;}
        }
        free(old); free(fresh);
    }
    for (size_t i = 0; ok && i < phase->cue_count; ++i) if (!used[i]) {
        cues[count] = phase->cues[i]; cues[count].resolved = false; cues[count].retired = true;
        cues[count].text = copy_text(phase->cues[i].text);
        if (!cues[count].text) ok = false;
        ++count;
    }
    free(used);
    if (!ok || count > FL_SPEECH_CUE_MAX) {fl_speech_cues_free(cues, capacity); return false;}
    for (size_t i = 0; i < op->cue_count; ++i) if (!cues[i].id) cues[i].id = (*next_id)++;
    fl_speech_cues_free((Fl_SpeechCue *)phase->cues, phase->cue_count);
    phase->cues = cues; phase->cue_count = count; phase->cue_asset = op->asset;
    return true;
}

bool fl_phase_speech_cue_sample(const Fl_Scene *scene, const Fl_Phase *phase, const Fl_SpeechCue *cue, bool end, Fl_Tick *out)
{
    if (!scene || !phase || !cue || !cue->resolved || cue->retired || !out || !phase->cue_asset) return false;
    for (size_t i = 0; i < scene->clip_count; ++i) {
        const Fl_AudioClip *clip = &scene->clips[i];
        if (clip->id != phase->clip || clip->asset != phase->cue_asset || clip->start.kind == FL_ANCHOR_SPEECH) continue;
        Fl_Tick base, local = end ? cue->end : cue->start;
        if (local < 0 || (uint64_t)local < clip->source_offset ||
            (clip->duration && (uint64_t)local - clip->source_offset > clip->duration) || !fl_anchor_sample(scene, &clip->start, &base)) return false;
        local -= (Fl_Tick)clip->source_offset;
        if (base > INT64_MAX - local) return false;
        *out = base + local; return true;
    }
    return false;
}

bool fl_speech_cue_sample(const Fl_Scene *scene, Fl_Id id, bool end, Fl_Tick *out)
{
    const Fl_Phase *phase = NULL; const Fl_SpeechCue *cue = fl_scene_speech_cue(scene,id,&phase);
    return fl_phase_speech_cue_sample(scene,phase,cue,end,out);
}

bool fl_speech_cues_valid(const Fl_Project *project, const Fl_Scene *scene, const Fl_Phase *phase, uint64_t next_id)
{
    (void)scene;
    if (phase->cue_count > FL_SPEECH_CUE_MAX || (phase->cue_count && (!phase->cues || !phase->cue_asset))) return false;
    const Fl_Asset *asset = phase->cue_asset ? fl_project_asset(project, phase->cue_asset) : NULL;
    if (phase->cue_asset && (!asset || asset->kind != FL_ASSET_AUDIO)) return false;
    for (size_t i = 0; i < phase->cue_count; ++i) {
        const Fl_SpeechCue *cue = &phase->cues[i];
        if (!cue->id || cue->id >= next_id || !cue->text || !cue->text[0] || strlen(cue->text) > 16384 ||
            !fl_json_utf8_valid(cue->text, strlen(cue->text)) || cue->start < 0 || cue->end < cue->start ||
            (cue->retired && cue->resolved) || (cue->resolved && cue->end <= cue->start) || (cue->kind != FL_CUE_WORD && cue->kind != FL_CUE_SENTENCE) ||
            (asset && cue->resolved && (uint64_t)cue->end > asset->duration)) return false;
    }
    return true;
}

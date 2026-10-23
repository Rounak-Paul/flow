#include "fl_voice.h"
#include "fl_audio.h"
#include "doc/fl_json.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "platform/fl_process.h"
#include "platform/fl_process_writer.h"
#include "core/fl_atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define VOICE_EXE ".exe"
#else
#define VOICE_EXE ""
#endif

/* Official llama.cpp GGUF of Qwen3-TTS 12Hz 1.7B Base. Apache-2.0. */
static const char *const k_voice_repo = "ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF";

static void note(char *error, size_t cap, const char *text)
{
    if (error && cap) snprintf(error, cap, "%s", text ? text : "");
}

static bool copy_str(char *out, size_t cap, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (!out || !cap || n >= cap) return false;
    memcpy(out, src, n + 1);
    return true;
}

static void prefer_flow_cache(const char *cache)
{
    if (!cache || !cache[0] || getenv("LLAMA_CACHE")) return;
#if defined(_WIN32)
    _putenv_s("LLAMA_CACHE", cache);
#else
    setenv("LLAMA_CACHE", cache, 0);
#endif
}

/* $FLOW_LLAMA_TTS, then the binary beside Flow. PATH is never searched. */
static bool find_runner(char *out, size_t cap)
{
    const char *override = getenv("FLOW_LLAMA_TTS");
    if (override && override[0] && fl_fs_exists(override)) return copy_str(out, cap, override);
    char dir[FL_PATH_MAX], candidate[FL_PATH_MAX];
    if (!fl_paths_executable_dir(dir, sizeof(dir))) return false;
    if (!fl_fs_join(candidate, sizeof(candidate), dir, "llama-tts" VOICE_EXE)) return false;
    return fl_fs_exists(candidate) && copy_str(out, cap, candidate);
}

static bool ends_with(const char *text, const char *suffix)
{
    size_t n = strlen(text), m = strlen(suffix);
    return n >= m && memcmp(text + n - m, suffix, m) == 0;
}

typedef struct GGUFScan {
    const char *dir;
    char model[FL_PATH_MAX];
    char mmproj[FL_PATH_MAX];
} GGUFScan;

static bool gguf_visit(const char *name, bool directory, void *user)
{
    GGUFScan *scan = user;
    if (directory || !ends_with(name, ".gguf")) return true;
    char path[FL_PATH_MAX];
    if (!fl_fs_join(path, sizeof(path), scan->dir, name)) return true;
    char *dest = strstr(name, "mmproj") ? scan->mmproj : scan->model;
    if (!dest[0]) memcpy(dest, path, strlen(path) + 1);
    return true;
}

static bool scan_gguf(const char *dir, GGUFScan *scan)
{
    memset(scan, 0, sizeof(*scan));
    scan->dir = dir;
    return fl_fs_list_dir(dir, gguf_visit, scan);
}

static bool parent_dir(char *out, size_t cap, const char *path)
{
    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *other = strrchr(path, '\\');
    if (!slash || (other && other > slash)) slash = other;
#endif
    if (!slash || slash == path) return false;
    size_t n = (size_t)(slash - path);
    if (n >= cap) return false;
    memcpy(out, path, n);
    out[n] = 0;
    return true;
}

typedef struct VoiceWeights {
    bool remote;
    char repo[FL_PATH_MAX];
    char model[FL_PATH_MAX];
    char mmproj[FL_PATH_MAX];
} VoiceWeights;

static bool weights_from_dir(const char *dir, VoiceWeights *weights, char *error, size_t error_cap)
{
    GGUFScan scan;
    if (!scan_gguf(dir, &scan) || !scan.model[0] || !scan.mmproj[0]) {
        note(error, error_cap, "The Qwen3-TTS folder needs a model .gguf and an mmproj .gguf.");
        return false;
    }
    weights->remote = false;
    return copy_str(weights->model, sizeof(weights->model), scan.model) &&
           copy_str(weights->mmproj, sizeof(weights->mmproj), scan.mmproj);
}

/* 1 when models/qwen3-tts beside Flow is a complete pair, 0 when that folder is absent, -1 when it is incomplete. */
static int bundled_weights(VoiceWeights *weights, char *error, size_t error_cap)
{
    char exe[FL_PATH_MAX], models[FL_PATH_MAX], folder[FL_PATH_MAX];
    if (!fl_paths_executable_dir(exe, sizeof(exe))) return 0;
    if (!fl_fs_join(models, sizeof(models), exe, "models")) return 0;
    if (!fl_fs_join(folder, sizeof(folder), models, "qwen3-tts")) return 0;
    if (!fl_fs_exists(folder)) return 0;
    GGUFScan scan;
    if (!scan_gguf(folder, &scan)) return 0;
    if (!scan.model[0] && !scan.mmproj[0]) return 0;
    if (!scan.model[0] || !scan.mmproj[0]) {
        note(error, error_cap, "The Qwen3-TTS folder needs a model .gguf and an mmproj .gguf.");
        return -1;
    }
    weights->remote = false;
    if (!copy_str(weights->model, sizeof(weights->model), scan.model) ||
        !copy_str(weights->mmproj, sizeof(weights->mmproj), scan.mmproj)) {
        note(error, error_cap, "Could not read the Qwen3-TTS model path.");
        return -1;
    }
    return 1;
}

/* Local files when they sit beside Flow or $FLOW_QWEN_TTS_MODEL names them. Otherwise the official repo. */
static bool resolve_weights(VoiceWeights *weights, char *error, size_t error_cap)
{
    memset(weights, 0, sizeof(*weights));
    const char *spec = getenv("FLOW_QWEN_TTS_MODEL");
    if (spec && spec[0]) {
        if (fl_fs_exists(spec)) {
            char dir[FL_PATH_MAX];
            GGUFScan file_scan;
            memset(&file_scan, 0, sizeof(file_scan));
            if (ends_with(spec, ".gguf")) {
                if (strstr(spec, "mmproj")) {
                    note(error, error_cap, "FLOW_QWEN_TTS_MODEL must be the talker .gguf, not the mmproj.");
                    return false;
                }
                if (!copy_str(weights->model, sizeof(weights->model), spec) || !parent_dir(dir, sizeof(dir), spec)) {
                    note(error, error_cap, "Could not read the Qwen3-TTS model path.");
                    return false;
                }
                file_scan.dir = dir;
                if (!fl_fs_list_dir(dir, gguf_visit, &file_scan) || !file_scan.mmproj[0]) {
                    note(error, error_cap, "The Qwen3-TTS folder needs an mmproj .gguf next to the model.");
                    return false;
                }
                weights->remote = false;
                return copy_str(weights->mmproj, sizeof(weights->mmproj), file_scan.mmproj);
            }
            return weights_from_dir(spec, weights, error, error_cap);
        }
        weights->remote = true;
        if (!copy_str(weights->repo, sizeof(weights->repo), spec)) {
            note(error, error_cap, "FLOW_QWEN_TTS_MODEL is too long.");
            return false;
        }
        return true;
    }
    int bundled = bundled_weights(weights, error, error_cap);
    if (bundled != 0) return bundled > 0;
    weights->remote = true;
    return copy_str(weights->repo, sizeof(weights->repo), k_voice_repo);
}

static const char *language_code(const char *language)
{
    static const struct {const char *name; const char *code;} table[] = {
        {"Chinese", "zh"}, {"English", "en"}, {"German", "de"}, {"Italian", "it"}, {"Portuguese", "pt"},
        {"Spanish", "es"}, {"Japanese", "ja"}, {"Korean", "ko"}, {"French", "fr"}, {"Russian", "ru"},
    };
    if (!language || !language[0] || strcmp(language, "Auto") == 0) return NULL;
    for (size_t i = 0; i < sizeof(table) / sizeof(*table); ++i)
        if (strcmp(language, table[i].name) == 0) return table[i].code;
    return NULL;
}

bool fl_voice_preset_path(const char *speaker, char *out, size_t cap)
{
    if (!fl_manuscript_speaker_ok(speaker) || !out || !cap) return false;
    char exe[FL_PATH_MAX], voices[FL_PATH_MAX];
    if (!fl_paths_executable_dir(exe, sizeof(exe))) return false;
    if (!fl_fs_join(voices, sizeof(voices), exe, "voices")) return false;
    static const char *const exts[] = {".wav", ".mp3"};
    for (size_t i = 0; i < sizeof(exts) / sizeof(*exts); ++i) {
        char name[FL_VOICE_NAME_MAX + 8];
        snprintf(name, sizeof(name), "%s%s", speaker, exts[i]);
        char path[FL_PATH_MAX];
        if (fl_fs_join(path, sizeof(path), voices, name) && fl_fs_exists(path)) return copy_str(out, cap, path);
    }
    return false;
}

static bool append_weights(char *command, size_t cap, const VoiceWeights *weights)
{
    if (weights->remote)
        return fl_command_append(command, cap, "-hf") && fl_command_append(command, cap, weights->repo);
    return fl_command_append(command, cap, "-m") && fl_command_append(command, cap, weights->model) &&
           fl_command_append(command, cap, "-mm") && fl_command_append(command, cap, weights->mmproj);
}

static Fl_VoiceStatus speak_chunk(const char *runner, const VoiceWeights *weights, const Fl_VoiceItem *item,
                                const atomic_bool *cancel, bool *limited, char *error, size_t error_cap)
{
    *limited = false;
    size_t bytes = 0;
    char *text = fl_fs_read_file(item->text_path, FL_PHASE_TEXT_MAX + 1, &bytes);
    if (!text) {note(error, error_cap, "Could not read the narration."); return FL_VOICE_FAILED;}
    if (!fl_manuscript_words(text)) {
        free(text);
        note(error, error_cap, "Nothing to speak.");
        return FL_VOICE_FAILED;
    }
    const char *speaker_file = NULL;
    char preset[FL_PATH_MAX];
    if (item->reference[0]) {
        if (!fl_fs_exists(item->reference)) {
            free(text);
            note(error, error_cap, "The voice recording is missing.");
            return FL_VOICE_FAILED;
        }
        speaker_file = item->reference;
    } else if (fl_voice_preset_path(item->speaker, preset, sizeof(preset))) speaker_file = preset;
    else {free(text); note(error, error_cap, "The built-in voice reference is missing. Rebuild Flow's voice resources."); return FL_VOICE_FAILED;}

    size_t cap = strlen(text) * 4 + FL_PATH_MAX * 8 + 512;
    char *command = calloc(1, cap);
    const char *ngl = getenv("FLOW_QWEN_TTS_NGL");
    if (!ngl || !ngl[0]) ngl = "99";
    const char *lang = language_code(item->language);
    bool built = command && fl_command_append(command, cap, runner) && append_weights(command, cap, weights) &&
                 fl_command_append(command, cap, "-ngl") && fl_command_append(command, cap, ngl) &&
                 fl_command_append(command, cap, "-n") && fl_command_append(command, cap, "2048");
    if (built && lang) built = fl_command_append(command, cap, "--tts-lang") && fl_command_append(command, cap, lang);
    if (built && speaker_file)
        built = fl_command_append(command, cap, "--tts-speaker-file") && fl_command_append(command, cap, speaker_file);
    built = built && fl_command_append(command, cap, "-p") && fl_command_append(command, cap, text) &&
            fl_command_append(command, cap, "-o") && fl_command_append(command, cap, item->out_path) &&
            strlen(command) + 6 < cap;
    free(text);
    if (!built) {
        free(command);
        note(error, error_cap, "Could not start Qwen3-TTS.");
        return FL_VOICE_FAILED;
    }
    char log[FL_PATH_MAX];
    int written = snprintf(log, sizeof(log), "%s.log", item->out_path);
    bool redirected = written > 0 && (size_t)written < sizeof(log) && strlen(command) + 3 < cap;
    if (redirected) {strcat(command, " >"); redirected = fl_command_append(command, cap, log) && strlen(command) + 6 < cap;}
    if (!redirected) {free(command); note(error, error_cap, "Could not prepare voice output."); return FL_VOICE_FAILED;}
    strcat(command, " 2>&1");
    Fl_ProcessWriter *process = fl_process_writer_open(command);
    free(command);
    if (!process) {note(error, error_cap, "Could not start Qwen3-TTS."); return FL_VOICE_FAILED;}
    bool success = fl_process_writer_wait(process, cancel);
    fl_process_writer_destroy(process);
    if (cancel && atomic_load_explicit(cancel, memory_order_relaxed)) return FL_VOICE_CANCELLED;
    if (!success) {
        char last[512] = {0};
        FILE *output = fl_fs_fopen(log, "rb");
        if (output) {
            if (fseek(output, 0, SEEK_END) == 0) {
                long length = ftell(output);
                if (length > 0 && fseek(output, length > 511 ? length - 511 : 0, SEEK_SET) == 0)
                    last[fread(last, 1, sizeof(last) - 1, output)] = 0;
            }
            fclose(output);
        }
        note(error, error_cap, last[0] ? last : "Qwen3-TTS failed.");
        return FL_VOICE_FAILED;
    }
    FILE *report = fl_fs_fopen(log, "rb");
    bool completed = false;
    if (report) {
        char line[2048];
        while (fgets(line, sizeof(line), report)) {
            const char *marker = strstr(line, "generated "); unsigned frames;
            if (marker && sscanf(marker, "generated %u frames,", &frames) == 1) {
                completed = true; *limited = frames >= 2048;
            }
        }
        fclose(report);
    }
    if (!completed || *limited) {
        note(error, error_cap, *limited ? "The voice part reached its generation limit." : "The voice runner did not confirm complete audio.");
        return FL_VOICE_FAILED;
    }
    if (!fl_fs_exists(item->out_path)) {
        note(error, error_cap, "Qwen3-TTS did not write the audio.");
        return FL_VOICE_FAILED;
    }
    return FL_VOICE_OK;
}

/* Choose a bounded UTF-8 slice, preferring sentence and then word boundaries; consume every byte. */
static size_t chunk_bytes(const char *text, size_t available, size_t maximum)
{
    size_t end = 0, characters = 0, sentence = 0, space = 0;
    while (end < available && characters < maximum) {
        size_t start = end++;
        while (end < available && ((unsigned char)text[end] & 0xc0) == 0x80) ++end;
        ++characters;
        if (text[start] == ' ' || text[start] == '\t' || text[start] == '\r') space = end;
        if (((text[start] == '.' || text[start] == '!' || text[start] == '?') &&
             (end == available || text[end] == ' ' || text[end] == '\n' || text[end] == '\r' || text[end] == '\t')) || text[start] == '\n' ||
            (end - start == 3 && (!memcmp(text + start, "。", 3) || !memcmp(text + start, "！", 3) || !memcmp(text + start, "？", 3)))) sentence = end;
    }
    if (end == available) return end;
    return sentence ? sentence : space ? space : end;
}

/* Encode a bounded unsigned integer into a WAV little-endian field. */
static void wav_integer(unsigned char *out, uint64_t value, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) out[i] = (unsigned char)(value >> (i * 8));
}

/* One worker-owned staging stream; no partial audio is published to the caller. */
typedef struct VoiceRender {
    const char *runner;
    const VoiceWeights *weights;
    const atomic_bool *cancel;
    Fl_VoiceProgress *progress;
    Fl_VoiceItem part;
    FILE *output;
    Fl_AudioInfo format;
    uint64_t frames;
    char *error;
    size_t error_cap;
} VoiceRender;

/* Decode and append one complete part without resampling, trimming or discarding samples. */
static bool append_part(VoiceRender *render)
{
    Fl_AudioInfo info;
    Fl_AudioReader *reader = fl_audio_open(render->part.out_path, &info);
    if (!reader) return false;
    bool ok = info.frames && info.channels <= 2;
    if (!render->format.sample_rate) render->format = info;
    ok = ok && info.sample_rate == render->format.sample_rate && info.channels == render->format.channels;
    float samples[2048]; unsigned char encoded[sizeof(samples)]; uint64_t frames = 0;
    while (ok) {
        if (render->cancel && atomic_load_explicit(render->cancel, memory_order_relaxed)) {ok = false; break;}
        uint64_t count = fl_audio_read(reader, samples, 2048 / info.channels);
        if (!count) break;
        size_t values = (size_t)count * info.channels;
        if (render->frames + count > (UINT32_MAX - 36u) / (info.channels * 4u)) {ok = false; break;}
        for (size_t i = 0; i < values; ++i) {
            uint32_t bits; memcpy(&bits, samples + i, sizeof(bits)); wav_integer(encoded + i * 4, bits, 4);
        }
        ok = fwrite(encoded, 4, values, render->output) == values;
        frames += count; render->frames += count;
    }
    fl_audio_close(reader);
    return ok && frames == info.frames;
}

/* Generate a range; split and retry a capped part instead of accepting truncated narration. */
static Fl_VoiceStatus render_range(VoiceRender *render, const char *text, size_t bytes)
{
    if (render->cancel && atomic_load_explicit(render->cancel, memory_order_relaxed)) return FL_VOICE_CANCELLED;
    char *part = malloc(bytes + 1);
    if (!part) {note(render->error, render->error_cap, "Out of memory preparing narration."); return FL_VOICE_FAILED;}
    memcpy(part, text, bytes); part[bytes] = 0;
    bool words = fl_manuscript_words(part) != 0;
    bool saved = !words || fl_fs_write_atomic(render->part.text_path, part, bytes);
    free(part);
    if (!saved) {note(render->error, render->error_cap, "Could not write a narration part."); return FL_VOICE_FAILED;}
    if (!words) {if (render->progress) fl_atomic_fetch_add_u32(&render->progress->done, 1); return FL_VOICE_OK;}
    fl_fs_remove(render->part.out_path);
    bool limited = false;
    Fl_VoiceStatus status = speak_chunk(render->runner, render->weights, &render->part, render->cancel, &limited, render->error, render->error_cap);
    if (limited) {
        size_t characters = 0;
        for (size_t i = 0; i < bytes; ++i) if (((unsigned char)text[i] & 0xc0) != 0x80) ++characters;
        if (characters < 2) return FL_VOICE_FAILED;
        size_t split = chunk_bytes(text, bytes, characters / 2);
        if (!split || split >= bytes) return FL_VOICE_FAILED;
        if (render->progress) fl_atomic_fetch_add_u32(&render->progress->total, 1);
        status = render_range(render, text, split);
        if (status == FL_VOICE_OK) status = render_range(render, text + split, bytes - split);
        return status;
    }
    if (status != FL_VOICE_OK) return status;
    if (render->error && render->error_cap) render->error[0] = 0;
    if (!append_part(render)) {
        if (render->cancel && atomic_load_explicit(render->cancel, memory_order_relaxed)) return FL_VOICE_CANCELLED;
        note(render->error, render->error_cap, "Could not stitch complete narration audio."); return FL_VOICE_FAILED;
    }
    if (render->progress) fl_atomic_fetch_add_u32(&render->progress->done, 1);
    return FL_VOICE_OK;
}

/* Render bounded parts to a private WAV and atomically publish only a fully stitched narration. */
static Fl_VoiceStatus speak_item(const char *runner, const VoiceWeights *weights, const Fl_VoiceItem *item,
                                 const char *text, const atomic_bool *cancel, Fl_VoiceProgress *progress, char *error, size_t error_cap)
{
    char directory[FL_PATH_MAX], staged[FL_PATH_MAX];
    int length = snprintf(directory, sizeof(directory), "%s.parts", item->out_path);
    if (length < 0 || (size_t)length >= sizeof(directory) || !fl_fs_mkdir_exclusive(directory)) {
        note(error, error_cap, "Could not prepare narration staging."); return FL_VOICE_FAILED;
    }
    VoiceRender render = {.runner = runner, .weights = weights, .cancel = cancel, .progress = progress,
        .part = *item, .error = error, .error_cap = error_cap};
    bool prepared = fl_fs_join(render.part.text_path, sizeof(render.part.text_path), directory, "text.txt") &&
        fl_fs_join(render.part.out_path, sizeof(render.part.out_path), directory, "part.wav") &&
        fl_fs_join(staged, sizeof(staged), directory, "stitched.wav");
    if (prepared) render.output = fl_fs_fopen(staged, "wbx");
    unsigned char header[44] = {0};
    Fl_VoiceStatus status = FL_VOICE_FAILED;
    if (!render.output || fwrite(header, 1, sizeof(header), render.output) != sizeof(header)) goto finish;
    size_t bytes = strlen(text);
    for (size_t offset = 0; offset < bytes;) {
        size_t length = chunk_bytes(text + offset, bytes - offset, 320);
        status = render_range(&render, text + offset, length);
        if (status != FL_VOICE_OK) goto finish;
        offset += length;
    }
    status = FL_VOICE_FAILED;
    if (!render.frames) goto finish;
    uint64_t data_size = render.frames * render.format.channels * 4;
    memcpy(header, "RIFF", 4); wav_integer(header + 4, data_size + 36, 4); memcpy(header + 8, "WAVEfmt ", 8);
    wav_integer(header + 16, 16, 4); wav_integer(header + 20, 3, 2); wav_integer(header + 22, render.format.channels, 2);
    wav_integer(header + 24, render.format.sample_rate, 4); wav_integer(header + 28, render.format.sample_rate * render.format.channels * 4u, 4);
    wav_integer(header + 32, render.format.channels * 4u, 2); wav_integer(header + 34, 32, 2);
    memcpy(header + 36, "data", 4); wav_integer(header + 40, data_size, 4);
    if (cancel && atomic_load_explicit(cancel, memory_order_relaxed)) {status = FL_VOICE_CANCELLED; goto finish;}
    if (fseek(render.output, 0, SEEK_SET) || fwrite(header, 1, sizeof(header), render.output) != sizeof(header) || !fl_fs_flush(render.output)) goto finish;
    if (fclose(render.output)) {render.output = NULL; goto finish;}
    render.output = NULL;
    if (!fl_fs_commit(staged, item->out_path)) goto finish;
    status = FL_VOICE_OK;
finish:
    if (render.output) fclose(render.output);
    fl_fs_remove_tree(directory);
    if (status == FL_VOICE_FAILED && (!error || !error_cap || !error[0])) note(error, error_cap, "Could not finish complete narration audio.");
    return status;
}

Fl_VoiceStatus fl_voice_run(const Fl_VoiceItem *items, size_t count, const atomic_bool *cancel, volatile uint32_t *done, Fl_VoiceProgress *progress,
                            char *error, size_t error_capacity)
{
    if (done) fl_atomic_store_u32(done, 0);
    if (progress) {fl_atomic_store_u32(&progress->done, 0); fl_atomic_store_u32(&progress->total, 0);}
    if (error && error_capacity) error[0] = 0;
    if (!items || !count) {note(error, error_capacity, "Nothing to speak."); return FL_VOICE_FAILED;}
    if (cancel && atomic_load_explicit(cancel, memory_order_relaxed)) return FL_VOICE_CANCELLED;
    char cache[FL_PATH_MAX], runner[FL_PATH_MAX];
    if (!fl_paths_user_dir(FL_PATH_CACHE, cache, sizeof(cache)) || !fl_fs_mkdir_p(cache)) {
        note(error, error_capacity, "Could not open the voice cache.");
        return FL_VOICE_FAILED;
    }
    prefer_flow_cache(cache);
    if (!find_runner(runner, sizeof(runner))) {
        note(error, error_capacity, "The Qwen3-TTS runner (llama-tts) was not found beside Flow.");
        return FL_VOICE_FAILED;
    }
    VoiceWeights weights;
    if (!resolve_weights(&weights, error, error_capacity)) return FL_VOICE_FAILED;
    char **texts = calloc(count, sizeof(*texts));
    if (!texts) {note(error, error_capacity, "Out of memory preparing narration."); return FL_VOICE_FAILED;}
    Fl_VoiceStatus result = FL_VOICE_FAILED;
    uint32_t parts = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t bytes;
        texts[i] = fl_fs_read_file(items[i].text_path, FL_PHASE_TEXT_MAX, &bytes);
        if (!texts[i] || !fl_json_utf8_valid(texts[i], bytes) || !fl_manuscript_words(texts[i])) {note(error, error_capacity, "Could not read complete narration text."); goto finish;}
        for (size_t offset = 0; offset < bytes; ++parts) offset += chunk_bytes(texts[i] + offset, bytes - offset, 320);
    }
    if (progress) fl_atomic_store_u32(&progress->total, parts);
    for (size_t i = 0; i < count; ++i) {
        result = speak_item(runner, &weights, &items[i], texts[i], cancel, progress, error, error_capacity);
        if (result != FL_VOICE_OK) goto finish;
        if (done) fl_atomic_store_u32(done, (uint32_t)(i + 1));
    }
finish:
    for (size_t i = 0; i < count; ++i) free(texts[i]);
    free(texts);
    if (result == FL_VOICE_OK && error && error_capacity) error[0] = 0;
    return result;
}

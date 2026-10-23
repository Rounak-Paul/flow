#include "fl_voice.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "platform/fl_process.h"
#include "core/fl_atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
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

static bool preset_speaker(char *out, size_t cap, const char *speaker)
{
    if (!speaker || !speaker[0]) return false;
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

static Fl_VoiceStatus speak_one(const char *runner, const VoiceWeights *weights, const Fl_VoiceItem *item,
                                char *error, size_t error_cap)
{
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
    } else if (preset_speaker(preset, sizeof(preset), item->speaker)) speaker_file = preset;

    size_t cap = strlen(text) * 4 + FL_PATH_MAX * 8 + 512;
    char *command = calloc(1, cap);
    const char *ngl = getenv("FLOW_QWEN_TTS_NGL");
    if (!ngl || !ngl[0]) ngl = "99";
    const char *lang = language_code(item->language);
    bool built = command && fl_command_append(command, cap, runner) && append_weights(command, cap, weights) &&
                 fl_command_append(command, cap, "-ngl") && fl_command_append(command, cap, ngl);
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
    strcat(command, " 2>&1");
    FILE *pipe = popen(command, "r");
    free(command);
    if (!pipe) {note(error, error_cap, "Could not start Qwen3-TTS."); return FL_VOICE_FAILED;}
    char line[1024], last[512];
    last[0] = 0;
    while (fgets(line, sizeof(line), pipe)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (line[0]) snprintf(last, sizeof(last), "%s", line);
    }
    int status = pclose(pipe);
    if (status != 0) {
        note(error, error_cap, last[0] ? last : "Qwen3-TTS failed.");
        return FL_VOICE_FAILED;
    }
    if (!fl_fs_exists(item->out_path)) {
        note(error, error_cap, "Qwen3-TTS did not write the audio.");
        return FL_VOICE_FAILED;
    }
    return FL_VOICE_OK;
}

Fl_VoiceStatus fl_voice_run(const Fl_VoiceItem *items, size_t count, volatile uint32_t *cancel, volatile uint32_t *done,
                            char *error, size_t error_capacity)
{
    if (done) fl_atomic_store_u32(done, 0);
    if (error && error_capacity) error[0] = 0;
    if (!items || !count) {note(error, error_capacity, "Nothing to speak."); return FL_VOICE_FAILED;}
    if (cancel && fl_atomic_load_u32(cancel)) return FL_VOICE_CANCELLED;
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
    for (size_t i = 0; i < count; ++i) {
        if (cancel && fl_atomic_load_u32(cancel)) return FL_VOICE_CANCELLED;
        Fl_VoiceStatus status = speak_one(runner, &weights, &items[i], error, error_capacity);
        if (status != FL_VOICE_OK) return status;
        if (done) fl_atomic_store_u32(done, (uint32_t)(i + 1));
        if (cancel && fl_atomic_load_u32(cancel)) return FL_VOICE_CANCELLED;
    }
    return FL_VOICE_OK;
}

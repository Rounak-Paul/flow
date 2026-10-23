#include "fl_speech_align.h"
#include "fl_audio.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "platform/fl_process.h"
#include "platform/fl_process_writer.h"
#include "doc/fl_json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Source byte span used only while building measured word/sentence cues. */
typedef struct WordSpan {size_t begin, end;} WordSpan;

/* Read one validated UTF-8 codepoint and advance cursor. */
static uint32_t codepoint(const char *text, size_t *cursor)
{
    unsigned char c = (unsigned char)text[(*cursor)++];
    if (c < 128) return c;
    unsigned n = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    uint32_t cp = c & (n == 1 ? 31u : n == 2 ? 15u : 7u);
    while (n--) cp = (cp << 6) | ((unsigned char)text[(*cursor)++] & 63u);
    return cp;
}

/* CJK writing does not require whitespace between individual alignment units. */
static bool cjk(uint32_t cp)
{
    return (cp >= 0x3400 && cp <= 0x9fff) || (cp >= 0x3040 && cp <= 0x30ff) ||
        (cp >= 0xac00 && cp <= 0xd7af) || (cp >= 0x20000 && cp <= 0x3134f);
}

/* Identify separators while retaining Latin letters, numbers and combining marks. */
static bool separator(uint32_t cp)
{
    return cp <= 32 || (cp < 128 && !((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
        (cp >= '0' && cp <= '9') || cp == '\'')) || (cp >= 0x2000 && cp <= 0x206f) ||
        (cp >= 0x3000 && cp <= 0x303f) || (cp >= 0xff01 && cp <= 0xff0f) || cp == 0xff1f || cp == 0xff1a;
}

/* Split validated UTF-8 text into exact source spans, preserving repeated word order. */
static size_t words(const char *text, WordSpan *spans)
{
    size_t cursor = 0, count = 0, start = SIZE_MAX;
    while (text[cursor]) {
        size_t begin = cursor; uint32_t cp = codepoint(text, &cursor);
        if (separator(cp) || cjk(cp)) {
            if (start != SIZE_MAX) {spans[count++] = (WordSpan){start, begin}; start = SIZE_MAX;}
            if (cjk(cp)) spans[count++] = (WordSpan){begin, cursor};
        } else if (start == SIZE_MAX) start = begin;
    }
    if (start != SIZE_MAX) spans[count++] = (WordSpan){start, cursor};
    return count;
}

/* Copy the exact source text for a cue label. */
static char *span_text(const char *text, size_t begin, size_t end)
{
    char *copy = malloc(end - begin + 1);
    if (copy) {memcpy(copy, text + begin, end - begin); copy[end - begin] = 0;}
    return copy;
}

void fl_speech_alignment_free(Fl_SpeechAlignment *alignment)
{
    if (!alignment) return;
    fl_speech_cues_free(alignment->cues, alignment->count); *alignment = (Fl_SpeechAlignment){0};
}

/* Decode one measured millisecond interval without inventing missing/invalid timestamps. */
static bool interval(const cJSON *item, Fl_Tick duration, Fl_Tick *start, Fl_Tick *end)
{
    const cJSON *offsets = cJSON_GetObjectItemCaseSensitive(item,"offsets");
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(offsets,"from"), *b = cJSON_GetObjectItemCaseSensitive(offsets,"to");
    return cJSON_IsNumber(a) && cJSON_IsNumber(b) && a->valuedouble >= 0 && b->valuedouble > a->valuedouble &&
        fl_time_from_seconds(a->valuedouble / 1000, start) && fl_time_from_seconds(b->valuedouble / 1000, end) && *end <= duration;
}

bool fl_speech_alignment_parse(const char *json, const char *script, Fl_Tick duration,
                               Fl_SpeechAlignment *out, char *error, size_t capacity)
{
    if (!json || !script || !out || duration <= 0 || strlen(script) > 16384 || !fl_json_utf8_valid(script,strlen(script))) return false;
    *out = (Fl_SpeechAlignment){0};
    Fl_Json context = {0}; cJSON *root = fl_json_parse(&context,json,strlen(json));
    const cJSON *segments = cJSON_GetObjectItemCaseSensitive(root,"transcription");
    if (!cJSON_IsArray(segments)) {cJSON_Delete(root); if (error && capacity) snprintf(error,capacity,"Speech analysis returned invalid timestamps."); return false;}
    WordSpan *spans = calloc(strlen(script) + 1,sizeof(*spans));
    Fl_SpeechCue *heard = calloc(32768,sizeof(*heard));
    if (!spans || !heard) {free(spans); free(heard); cJSON_Delete(root); return false;}
    size_t count = words(script,spans), heard_count = 0;
    Fl_SpeechCue *cues = count ? calloc(count * 2,sizeof(*cues)) : NULL;
    bool ok = count && cues;
    for (size_t i = 0; ok && i < count; ++i) {
        cues[i].text = span_text(script,spans[i].begin,spans[i].end);
        if (!cues[i].text) ok = false;
    }
    for (const cJSON *segment = segments->child; ok && segment; segment = segment->next) {
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(segment,"text");
        if (!cJSON_IsString(text) || strlen(text->valuestring) > 16384 || !fl_json_utf8_valid(text->valuestring,strlen(text->valuestring))) {ok = false; break;}
        WordSpan *tokens = calloc(strlen(text->valuestring) + 1,sizeof(*tokens));
        if (!tokens) {ok = false; break;}
        size_t n = words(text->valuestring,tokens); Fl_Tick start = 0, end = 0;
        bool resolved = interval(segment,duration,&start,&end);
        if (!resolved) start = end = 0;
        const cJSON *probabilities = cJSON_GetObjectItemCaseSensitive(segment,"tokens");
        for (const cJSON *token = probabilities ? probabilities->child : NULL; token; token = token->next) {
            const cJSON *p = cJSON_GetObjectItemCaseSensitive(token,"p");
            const cJSON *label = cJSON_GetObjectItemCaseSensitive(token,"text");
            if (cJSON_IsNumber(p) && p->valuedouble < 0.25 && (!cJSON_IsString(label) || (strncmp(label->valuestring,"[_",2) && strncmp(label->valuestring,"<|",2)))) resolved = false;
        }
        if (n > 32768 - heard_count) ok = false;
        for (size_t j = 0; ok && j < n; ++j) {
            Fl_SpeechCue *cue = &heard[heard_count++]; cue->text = span_text(text->valuestring,tokens[j].begin,tokens[j].end);
            cue->start = start; cue->end = end; cue->resolved = resolved;
            if (!cue->text) ok = false;
        }
        free(tokens);
    }
    /* Preserve exact runs first; only unique monotonic matches cross recognition disagreements. */
    if (ok) {
        size_t begin = 0, audio_begin = 0, end = count, audio_end = heard_count;
        while (begin < end && audio_begin < audio_end && fl_speech_cue_equal(&cues[begin],&heard[audio_begin])) {
            cues[begin].start = heard[audio_begin].start; cues[begin].end = heard[audio_begin].end;
            cues[begin].resolved = heard[audio_begin].resolved; ++begin; ++audio_begin;
        }
        while (begin < end && audio_begin < audio_end && fl_speech_cue_equal(&cues[end - 1],&heard[audio_end - 1])) {
            --end; --audio_end; cues[end].start = heard[audio_end].start; cues[end].end = heard[audio_end].end;
            cues[end].resolved = heard[audio_end].resolved;
        }
        size_t last = audio_begin;
        for (size_t i = begin; i < end; ++i) {
            size_t match = SIZE_MAX, found = 0, source_occurrences = 0;
            for (size_t j = last; j < audio_end; ++j) if (fl_speech_cue_equal(&cues[i],&heard[j])) {match = j; ++found;}
            for (size_t j = begin; j < end; ++j) source_occurrences += fl_speech_cue_equal(&cues[i],&cues[j]);
            if (found == 1 && source_occurrences == 1) {
                cues[i].start = heard[match].start; cues[i].end = heard[match].end; cues[i].resolved = heard[match].resolved; last = match + 1;
            }
        }
    }
    for (size_t i = 0; ok && i < count; ++i) if (cues[i].end > cues[i].start) {
        size_t source_count = 0, audio_count = 0;
        for (size_t j = 0; j < count; ++j) source_count += fl_speech_cue_equal(&cues[i],&cues[j]);
        for (size_t j = 0; j < heard_count; ++j) audio_count += fl_speech_cue_equal(&cues[i],&heard[j]);
        if (source_count != audio_count && (source_count > 1 || audio_count > 1)) {
            cues[i].resolved = false; cues[i].start = cues[i].end = 0;
        }
    }
    size_t cue_count = count;
    for (size_t begin = 0, i = 0; ok && i < count; ++i) {
        size_t finish = i + 1 < count ? spans[i + 1].begin : strlen(script);
        bool boundary = i + 1 == count;
        for (size_t at = spans[i].end; at < finish;) {
            uint32_t cp = codepoint(script,&at);
            if (cp == '.' || cp == '!' || cp == '?' || cp == '\n' || cp == 0x3002 || cp == 0xff01 || cp == 0xff1f) boundary = true;
        }
        if (!boundary) continue;
        Fl_SpeechCue *sentence = &cues[cue_count++]; sentence->kind = FL_CUE_SENTENCE;
        sentence->text = span_text(script,spans[begin].begin,spans[i].end);
        sentence->start = cues[begin].start; sentence->end = cues[i].end; sentence->resolved = true;
        for (size_t k = begin; k <= i; ++k) if (!cues[k].resolved) sentence->resolved = false;
        bool measured = true;
        for (size_t k = begin; k <= i; ++k) if (cues[k].end <= cues[k].start ||
            (k > begin && cues[k].start < cues[k - 1].start)) measured = false;
        if (!measured) {sentence->start = sentence->end = 0; sentence->resolved = false;}
        if (!sentence->text) ok = false;
        begin = i + 1;
    }
    free(spans); fl_speech_cues_free(heard,heard_count); cJSON_Delete(root);
    if (!ok) {fl_speech_cues_free(cues,count * 2); if (error && capacity) snprintf(error,capacity,"Could not align this narration within the speech-analysis limits."); return false;}
    out->cues = cues; out->count = cue_count; return true;
}

/* Write a little-endian PCM16 WAV field. */
static void integer(unsigned char *out, uint64_t value, size_t size)
{
    for (size_t i = 0; i < size; ++i) out[i] = (unsigned char)(value >> (8 * i));
}

/* Convert generated audio to bounded mono 16 kHz PCM for the local recognizer. */
static bool analysis_audio(const char *source, const char *path, const atomic_bool *cancel, Fl_Tick *duration)
{
    Fl_AudioInfo info; Fl_AudioReader *reader = fl_audio_open_resampled(source,&info,16000,1);
    if (!reader) return false;
    FILE *file = fl_fs_fopen(path,"wbx");
    bool ok = file && info.frames && info.frames <= (UINT32_MAX - 36u) / 2;
    unsigned char header[44] = {0}; float samples[4096]; unsigned char pcm[8192]; uint64_t frames = 0;
    if (ok) ok = fwrite(header,1,sizeof(header),file) == sizeof(header);
    while (ok) {
        if (cancel && atomic_load_explicit(cancel,memory_order_relaxed)) {ok = false; break;}
        uint64_t n = fl_audio_read(reader,samples,4096); if (!n) break;
        for (size_t i = 0; i < n; ++i) {
            if (!isfinite(samples[i])) {ok = false; break;}
            int value = (int)lrintf(fmaxf(-1,fminf(1,samples[i])) * 32767);
            integer(pcm + i * 2,(uint16_t)value,2);
        }
        if (ok) ok = fwrite(pcm,2,(size_t)n,file) == n;
        frames += n;
    }
    ok = ok && frames == info.frames;
    if (ok) {
        memcpy(header,"RIFF",4); integer(header + 4,frames * 2 + 36,4); memcpy(header + 8,"WAVEfmt ",8);
        integer(header + 16,16,4); integer(header + 20,1,2); integer(header + 22,1,2);
        integer(header + 24,16000,4); integer(header + 28,32000,4); integer(header + 32,2,2); integer(header + 34,16,2);
        memcpy(header + 36,"data",4); integer(header + 40,frames * 2,4);
        ok = fseek(file,0,SEEK_SET) == 0 && fwrite(header,1,sizeof(header),file) == sizeof(header) &&
            fl_time_from_seconds((double)frames / 16000,duration);
    }
    if (file && fclose(file)) ok = false;
    fl_audio_close(reader); return ok;
}

bool fl_speech_align(const char *audio, const char *script, const char *language, const char *work,
                     const atomic_bool *cancel, Fl_SpeechAlignment *out, char *error, size_t capacity)
{
    if (!out) return false;
    *out = (Fl_SpeechAlignment){0};
    char executable[FL_PATH_MAX], runner[FL_PATH_MAX], model[FL_PATH_MAX], input[FL_PATH_MAX], prefix[FL_PATH_MAX], json[FL_PATH_MAX], log[FL_PATH_MAX];
    const char *override = getenv("FLOW_WHISPER"), *weights = getenv("FLOW_WHISPER_MODEL");
#ifdef _WIN32
    const char *name = "whisper-cli.exe";
#else
    const char *name = "whisper-cli";
#endif
    bool ok = (!override || strlen(override) < sizeof(runner)) && (!weights || strlen(weights) < sizeof(model)) && fl_paths_executable_dir(executable,sizeof(executable)) &&
        (override ? snprintf(runner,sizeof(runner),"%s",override) > 0 : fl_fs_join(runner,sizeof(runner),executable,name)) &&
        (weights ? snprintf(model,sizeof(model),"%s",weights) > 0 : fl_fs_join(model,sizeof(model),executable,"models/ggml-base.bin")) &&
        fl_fs_exists(runner) && fl_fs_exists(model) && fl_fs_join(input,sizeof(input),work,"align.wav") &&
        fl_fs_join(prefix,sizeof(prefix),work,"alignment") && fl_fs_join(json,sizeof(json),work,"alignment.json") && fl_fs_join(log,sizeof(log),work,"alignment.log");
    if (!ok) {if (error && capacity) snprintf(error,capacity,"The bundled speech timing runner or model is missing."); return false;}
    Fl_Tick duration;
    if (!analysis_audio(audio,input,cancel,&duration)) {if (error && capacity) snprintf(error,capacity,"Could not prepare narration for speech timing."); return false;}
    static const char *const languages[] = {"Auto","Chinese","English","Japanese","Korean","German","French","Russian","Portuguese","Spanish","Italian"};
    static const char *const codes[] = {"auto","zh","en","ja","ko","de","fr","ru","pt","es","it"};
    const char *code = "auto";
    for (size_t i = 0; i < sizeof(languages) / sizeof(*languages); ++i) if (language && !strcmp(language,languages[i])) code = codes[i];
    char command[FL_PATH_MAX * 8] = {0};
    const char *args[] = {runner,"-m",model,"-f",input,"-l",code,"-of",prefix,"-ojf","-ml","1","-sow","-dtw","base"};
    for (size_t i = 0; ok && i < sizeof(args) / sizeof(*args); ++i) ok = fl_command_append(command,sizeof(command),args[i]);
    if (ok && strlen(command) + 3 < sizeof(command)) {strcat(command," >"); ok = fl_command_append(command,sizeof(command),log) && strlen(command) + 6 < sizeof(command);}
    else ok = false;
    if (ok) strcat(command," 2>&1");
    Fl_ProcessWriter *process = ok ? fl_process_writer_open(command) : NULL;
    ok = process && fl_process_writer_wait(process,cancel); fl_process_writer_destroy(process);
    char *measured = ok ? fl_fs_read_file(json,32u * 1024u * 1024u,NULL) : NULL;
    ok = measured && fl_speech_alignment_parse(measured,script,duration,out,error,capacity);
    free(measured);
    if (!ok && error && capacity && !error[0]) snprintf(error,capacity,"Speech timing analysis failed. No estimated cues were created.");
    return ok;
}

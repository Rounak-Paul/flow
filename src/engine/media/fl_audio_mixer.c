#include "fl_audio_mixer.h"
#include "fl_audio.h"
#include "fl_miniaudio.h"
#include "core/fl_atomic.h"
#include "core/fl_log.h"
#include "core/fl_thread.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "mixer"
#define OUTPUT_CHANNELS 2
#define CHUNK_FRAMES 1024
#define SCRATCH_FRAMES 4096

typedef struct Fl_Voice {
    Fl_AudioReader *reader;
    ma_data_converter converter;    /* initialised in place; never copied */
    float *input;
    uint64_t input_frames, input_offset;
    uint32_t channels;
    int64_t start;                  /* device frame (relative to the mixer start) of the voice's first output frame */
    int64_t length;                 /* device frames to play; INT64_MAX until the file ends */
    int64_t played;                 /* device frames produced so far */
    int64_t clip_offset;            /* device frames of the clip already skipped before the start time */
    int64_t clip_length, fade_in, fade_out;
    double gain;
    bool done;
} Fl_Voice;
struct Fl_AudioMixer {
    bool silent, device_ready, running;
    ma_context context;
    ma_device device;
    Fl_Mutex lock;
    Fl_Voice *voices;
    size_t voice_count;
    float *scratch;
    Fl_Tick from;
    volatile uint64_t rendered;     /* device frames since start; read lock-free by the clock */
};

/* Release every voice; caller holds the lock (or the device is stopped). */
static void release_voices(Fl_AudioMixer *m)
{
    for (size_t i = 0; i < m->voice_count; ++i) {
        ma_data_converter_uninit(&m->voices[i].converter, NULL);
        fl_audio_close(m->voices[i].reader);
        free(m->voices[i].input);
    }
    free(m->voices);
    m->voices = NULL; m->voice_count = 0;
}

/* Linear fade gain of a clip-relative device frame. */
static double fade(const Fl_Voice *v, int64_t at)
{
    double g = v->gain;
    if (v->fade_in > 0 && at < v->fade_in) g *= (double)at / (double)v->fade_in;
    if (v->fade_out > 0 && v->clip_length != INT64_MAX && at > v->clip_length - v->fade_out)
        g *= fmax(0, (double)(v->clip_length - at) / (double)v->fade_out);
    return g;
}

/* Produce up to frames converted frames of a voice into out; returns frames produced. */
static uint64_t pull(Fl_Voice *v, float *out, uint64_t frames)
{
    uint64_t made = 0;
    while (made < frames && !v->done) {
        if (v->input_offset == v->input_frames) {
            v->input_offset = 0;
            v->input_frames = fl_audio_read(v->reader, v->input, CHUNK_FRAMES);
            if (!v->input_frames) { v->done = true; break; }
        }
        ma_uint64 in = v->input_frames - v->input_offset, produced = frames - made;
        if (ma_data_converter_process_pcm_frames(&v->converter, v->input + v->input_offset * v->channels, &in,
                                                 out + made * OUTPUT_CHANNELS, &produced) != MA_SUCCESS || (!in && !produced)) {
            v->done = true; break;
        }
        v->input_offset += in; made += produced;
    }
    return made;
}

/* Device thread: mix every voice overlapping this period. */
static void render(ma_device *device, void *output, const void *input, ma_uint32 frames)
{
    (void)input;
    Fl_AudioMixer *m = device->pUserData;
    float *out = output;
    memset(out, 0, (size_t)frames * OUTPUT_CHANNELS * sizeof(float));
    fl_mutex_lock(&m->lock);
    int64_t base = (int64_t)fl_atomic_load_u64(&m->rendered);
    for (size_t i = 0; m->running && i < m->voice_count; ++i) {
        Fl_Voice *v = &m->voices[i];
        for (int64_t at = base; !v->done && at < base + frames; ) {
            int64_t local = at - v->start;
            if (local < 0) { at = v->start; continue; }
            if (v->played >= v->length) { v->done = true; break; }
            int64_t want = base + frames - at;
            if (want > SCRATCH_FRAMES) want = SCRATCH_FRAMES;
            if (want > v->length - v->played) want = v->length - v->played;
            uint64_t got = pull(v, m->scratch, (uint64_t)want);
            for (uint64_t f = 0; f < got; ++f) {
                float g = (float)fade(v, v->clip_offset + v->played + (int64_t)f);
                float *dst = out + (size_t)(at - base + (int64_t)f) * OUTPUT_CHANNELS;
                dst[0] += m->scratch[f * 2] * g; dst[1] += m->scratch[f * 2 + 1] * g;
            }
            v->played += (int64_t)got; at += (int64_t)got;
            if (!got) break;
        }
    }
    for (size_t i = 0; i < (size_t)frames * OUTPUT_CHANNELS; ++i) out[i] = fmaxf(-1.0f, fminf(1.0f, out[i]));
    fl_atomic_fetch_add_u64(&m->rendered, frames);
    fl_mutex_unlock(&m->lock);
}

/* Open the context/device on first use. */
static bool open_device(Fl_AudioMixer *m)
{
    if (m->device_ready) return true;
    ma_backend null_backend = ma_backend_null;
    if (ma_context_init(m->silent ? &null_backend : NULL, m->silent ? 1 : 0, NULL, &m->context) != MA_SUCCESS) return false;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = OUTPUT_CHANNELS;
    config.dataCallback = render;
    config.pUserData = m;
    if (ma_device_init(&m->context, &config, &m->device) != MA_SUCCESS) { ma_context_uninit(&m->context); return false; }
    m->device_ready = true;
    return true;
}

Fl_AudioMixer *fl_audio_mixer_create(const Fl_AudioMixerDesc *desc)
{
    Fl_AudioMixer *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->silent = desc && desc->silent;
    m->scratch = malloc((size_t)SCRATCH_FRAMES * OUTPUT_CHANNELS * sizeof(float));
    if (!m->scratch || !fl_mutex_init(&m->lock)) { free(m->scratch); free(m); return NULL; }
    return m;
}

void fl_audio_mixer_destroy(Fl_AudioMixer *m)
{
    if (!m) return;
    if (m->device_ready) { ma_device_uninit(&m->device); ma_context_uninit(&m->context); }
    release_voices(m);
    fl_mutex_destroy(&m->lock);
    free(m->scratch); free(m);
}

/* Convert ticks to frames at rate, rounding down. */
static int64_t frames_of(Fl_Tick ticks, uint32_t rate)
{
    return ticks <= 0 ? 0 : (int64_t)((double)ticks * rate / FL_TICKS_PER_SECOND);
}

/* Open and position one voice in place; false when the clip cannot play from this start. */
static bool prepare_voice(Fl_Voice *v, const Fl_MixerClip *c, Fl_Tick from, uint32_t rate)
{
    Fl_AudioInfo info;
    *v = (Fl_Voice){0};
    Fl_Tick skipped = from > c->start ? from - c->start : 0;
    if (c->duration && skipped >= c->duration) return false;
    v->reader = fl_audio_open(c->path, &info);
    if (!v->reader) return false;
    uint64_t source = (uint64_t)frames_of(c->source_offset + skipped, info.sample_rate);
    if (info.frames && source >= info.frames) { fl_audio_close(v->reader); return false; }
    v->input = malloc((size_t)CHUNK_FRAMES * info.channels * sizeof(float));
    ma_data_converter_config config = ma_data_converter_config_init(ma_format_f32, ma_format_f32, info.channels,
        OUTPUT_CHANNELS, info.sample_rate, rate);
    config.resampling.algorithm = ma_resample_algorithm_linear;
    if (!v->input || (source && !fl_audio_seek(v->reader, source)) || ma_data_converter_init(&config, NULL, &v->converter) != MA_SUCCESS) {
        free(v->input); fl_audio_close(v->reader); *v = (Fl_Voice){0}; return false;
    }
    v->channels = info.channels;
    v->start = c->start > from ? frames_of(c->start - from, rate) : 0;
    v->clip_offset = frames_of(skipped, rate);
    v->clip_length = c->duration ? frames_of(c->duration, rate) : INT64_MAX;
    v->length = c->duration ? v->clip_length - v->clip_offset : INT64_MAX;
    v->fade_in = frames_of(c->fade_in, rate); v->fade_out = frames_of(c->fade_out, rate);
    v->gain = c->gain;
    return true;
}

bool fl_audio_mixer_start(Fl_AudioMixer *m, const Fl_MixerClip *clips, size_t count, Fl_Tick from)
{
    if (!m || (count && !clips)) return false;
    fl_audio_mixer_stop(m);
    if (!open_device(m)) { FL_LOG_WARN(LOG_TAG, "audio output unavailable; playing without sound"); return false; }
    uint32_t rate = m->device.sampleRate;
    Fl_Voice *voices = count ? calloc(count, sizeof(*voices)) : NULL;
    if (count && !voices) return false;
    size_t ready = 0;
    for (size_t i = 0; i < count; ++i) {
        if (prepare_voice(&voices[ready], &clips[i], from, rate)) ++ready;
        else if (!clips[i].duration || from - clips[i].start < clips[i].duration) FL_LOG_WARN(LOG_TAG, "skipping clip %s", clips[i].path);
    }
    fl_mutex_lock(&m->lock);
    m->voices = voices; m->voice_count = ready; m->from = from; m->running = true;
    fl_atomic_store_u64(&m->rendered, 0);
    fl_mutex_unlock(&m->lock);
    if (ma_device_start(&m->device) != MA_SUCCESS) { fl_audio_mixer_stop(m); return false; }
    return true;
}

void fl_audio_mixer_stop(Fl_AudioMixer *m)
{
    if (!m) return;
    if (m->device_ready) ma_device_stop(&m->device);
    fl_mutex_lock(&m->lock);
    m->running = false;
    release_voices(m);
    fl_mutex_unlock(&m->lock);
}

bool fl_audio_mixer_position(Fl_AudioMixer *m, Fl_Tick *tick)
{
    if (!m || !m->running || !tick || !m->device_ready || !m->device.sampleRate) return false;
    uint64_t frames = fl_atomic_load_u64(&m->rendered);
    *tick = m->from + (Fl_Tick)((double)frames * FL_TICKS_PER_SECOND / m->device.sampleRate);
    return true;
}

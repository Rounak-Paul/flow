#include "fl_audio_player.h"
#include "fl_audio.h"
#include "fl_miniaudio.h"
#include "core/fl_log.h"
#include "core/fl_thread.h"
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "audio"
#define OUTPUT_CHANNELS 2
#define CHUNK_FRAMES 1024

struct Fl_AudioPlayer {
    Fl_EventBus *bus;
    Fl_EventType event;
    bool silent, device_ready;
    ma_context context;
    ma_device device;
    Fl_Mutex lock;                  /* guards everything below against the device thread */
    Fl_AudioReader *reader;
    ma_data_converter converter;
    bool converter_ready, finished;
    float *input;                   /* decoded, not yet converted frames */
    uint64_t input_frames, input_offset, input_capacity;
    uint32_t input_channels;
    uint64_t token, next_token;
};

/* Release the current source; caller holds the lock. */
static void release_source(Fl_AudioPlayer *p)
{
    if (p->converter_ready) ma_data_converter_uninit(&p->converter, NULL);
    fl_audio_close(p->reader);
    p->reader = NULL; p->converter_ready = false; p->finished = false;
    p->input_frames = p->input_offset = 0; p->token = 0;
}

/* Device thread: decode, convert to the device format and mix silence after the end. */
static void render(ma_device *device, void *output, const void *input, ma_uint32 frames)
{
    (void)input;
    Fl_AudioPlayer *p = device->pUserData;
    float *out = output;
    ma_uint32 channels = device->playback.channels;
    memset(out, 0, (size_t)frames * channels * sizeof(float));
    fl_mutex_lock(&p->lock);
    ma_uint64 written = 0;
    while (p->reader && !p->finished && written < frames) {
        if (p->input_offset == p->input_frames) {
            p->input_offset = 0;
            p->input_frames = fl_audio_read(p->reader, p->input, CHUNK_FRAMES);
            if (!p->input_frames) {
                p->finished = true;
                Fl_AudioPlayback ended = {p, p->token, false};
                fl_event_post(p->bus, p->event, &ended, sizeof(ended));
                break;
            }
        }
        ma_uint64 in = p->input_frames - p->input_offset, made = frames - written;
        if (ma_data_converter_process_pcm_frames(&p->converter, p->input + p->input_offset * p->input_channels, &in,
                                                 out + written * channels, &made) != MA_SUCCESS || (!in && !made)) {
            p->finished = true;
            Fl_AudioPlayback ended = {p, p->token, false};
            fl_event_post(p->bus, p->event, &ended, sizeof(ended));
            break;
        }
        p->input_offset += in; written += made;
    }
    fl_mutex_unlock(&p->lock);
}

/* Owner thread: free the finished source and park the device. */
static bool playback_changed(const Fl_Event *event, void *data)
{
    Fl_AudioPlayer *p = data;
    const Fl_AudioPlayback *change = event->payload;
    if (change->player != p || change->playing) return false;
    fl_mutex_lock(&p->lock);
    bool current = p->token == change->token && p->finished;
    if (current) release_source(p);
    fl_mutex_unlock(&p->lock);
    if (current && p->device_ready) ma_device_stop(&p->device);
    return false;
}

/* Open the output context/device on first use. */
static bool open_device(Fl_AudioPlayer *p)
{
    if (p->device_ready) return true;
    ma_backend null_backend = ma_backend_null;
    if (ma_context_init(p->silent ? &null_backend : NULL, p->silent ? 1 : 0, NULL, &p->context) != MA_SUCCESS) {
        FL_LOG_WARN(LOG_TAG, "no audio backend available"); return false;
    }
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = OUTPUT_CHANNELS;
    config.sampleRate = 0;
    config.dataCallback = render;
    config.pUserData = p;
    if (ma_device_init(&p->context, &config, &p->device) != MA_SUCCESS) {
        ma_context_uninit(&p->context);
        FL_LOG_WARN(LOG_TAG, "audio output device unavailable"); return false;
    }
    p->device_ready = true;
    return true;
}

Fl_AudioPlayer *fl_audio_player_create(const Fl_AudioPlayerDesc *desc)
{
    if (!desc || !desc->bus) return NULL;
    Fl_AudioPlayer *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->bus = desc->bus; p->silent = desc->silent; p->next_token = 1;
    p->event = fl_event_type_register(p->bus, FL_EVENT_AUDIO_PLAYBACK, sizeof(Fl_AudioPlayback));
    if (p->event == FL_EVENT_TYPE_INVALID || !fl_mutex_init(&p->lock)) { free(p); return NULL; }
    if (!fl_event_subscribe(p->bus, &(Fl_SubscribeDesc){.type = p->event, .fn = playback_changed, .user_data = p,
                                                         .owner = p, .priority = 1000}).generation) {
        fl_mutex_destroy(&p->lock); free(p); return NULL;
    }
    return p;
}

void fl_audio_player_destroy(Fl_AudioPlayer *p)
{
    if (!p) return;
    fl_event_unsubscribe_owner(p->bus, p);
    if (p->device_ready) { ma_device_uninit(&p->device); ma_context_uninit(&p->context); }
    release_source(p);
    free(p->input);
    fl_mutex_destroy(&p->lock);
    free(p);
}

uint64_t fl_audio_player_play(Fl_AudioPlayer *p, const char *path)
{
    if (!p || !path || !open_device(p)) return 0;
    Fl_AudioInfo info;
    Fl_AudioReader *reader = fl_audio_open(path, &info);
    if (!reader) return 0;
    ma_data_converter_config config = ma_data_converter_config_init(ma_format_f32, ma_format_f32, info.channels,
        p->device.playback.channels, info.sample_rate, p->device.sampleRate);
    config.resampling.algorithm = ma_resample_algorithm_linear;
    uint64_t capacity = (uint64_t)CHUNK_FRAMES * info.channels;
    fl_mutex_lock(&p->lock);
    release_source(p);
    float *input = capacity > p->input_capacity ? realloc(p->input, capacity * sizeof(float)) : p->input;
    if (input && input != p->input) { p->input = input; p->input_capacity = capacity; }
    /* The converter holds pointers into itself, so it is initialised in place, never copied. */
    if (!input || ma_data_converter_init(&config, NULL, &p->converter) != MA_SUCCESS) {
        fl_mutex_unlock(&p->lock); fl_audio_close(reader); return 0;
    }
    p->reader = reader; p->converter_ready = true; p->input_channels = info.channels;
    p->token = p->next_token++;
    uint64_t token = p->token;
    fl_mutex_unlock(&p->lock);
    if (ma_device_get_state(&p->device) != ma_device_state_started && ma_device_start(&p->device) != MA_SUCCESS) {
        fl_mutex_lock(&p->lock); release_source(p); fl_mutex_unlock(&p->lock);
        FL_LOG_WARN(LOG_TAG, "audio output could not start"); return 0;
    }
    return token;
}

void fl_audio_player_stop(Fl_AudioPlayer *p)
{
    if (!p) return;
    fl_mutex_lock(&p->lock);
    release_source(p);
    fl_mutex_unlock(&p->lock);
    if (p->device_ready) ma_device_stop(&p->device);
}

uint64_t fl_audio_player_current(const Fl_AudioPlayer *p)
{
    if (!p) return 0;
    fl_mutex_lock((Fl_Mutex *)&p->lock);
    uint64_t token = p->reader && !p->finished ? p->token : 0;
    fl_mutex_unlock((Fl_Mutex *)&p->lock);
    return token;
}

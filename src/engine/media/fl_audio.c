#include "fl_audio.h"
#include "fl_miniaudio.h"
#include "platform/fl_audio_file.h"
#include "platform/fl_fs.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_FRAMES 4096

struct Fl_AudioReader {
    FILE *file;                     /* miniaudio source; NULL when the platform decoder is used */
    ma_decoder decoder;
    Fl_AudioFile *platform;
    Fl_AudioInfo info;
};

/* miniaudio read callback over a UTF-8-opened stdio stream. */
static ma_result stream_read(ma_decoder *decoder, void *buffer, size_t bytes, size_t *read)
{
    FILE *file = decoder->pUserData;
    *read = fread(buffer, 1, bytes, file);
    if (*read < bytes && ferror(file)) return MA_IO_ERROR;
    return *read || !bytes ? MA_SUCCESS : MA_AT_END;
}

/* miniaudio seek callback; offsets beyond long are rejected. */
static ma_result stream_seek(ma_decoder *decoder, ma_int64 offset, ma_seek_origin origin)
{
    FILE *file = decoder->pUserData;
    if (offset > LONG_MAX || offset < LONG_MIN) return MA_INVALID_ARGS;
    int whence = origin == ma_seek_origin_start ? SEEK_SET : origin == ma_seek_origin_end ? SEEK_END : SEEK_CUR;
    return fseek(file, (long)offset, whence) == 0 ? MA_SUCCESS : MA_IO_ERROR;
}

/* Accept only stream descriptions downstream code can size buffers for. */
static bool sane(const Fl_AudioInfo *info)
{
    return info->sample_rate >= 1000 && info->sample_rate <= 768000 && info->channels >= 1 && info->channels <= 32;
}

/* Open a decoder with an optional requested PCM rate/channel layout. */
static Fl_AudioReader *open_format(const char *path, Fl_AudioInfo *info, uint32_t rate, uint32_t channels)
{
    if (!path || !info) return NULL;
    Fl_AudioReader *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->file = fl_fs_fopen(path, "rb");
    if (r->file) {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, channels, rate);
        if (ma_decoder_init(stream_read, stream_seek, r->file, &config, &r->decoder) == MA_SUCCESS) {
            ma_uint64 length = 0;
            r->info = (Fl_AudioInfo){r->decoder.outputSampleRate, r->decoder.outputChannels, 0};
            if (ma_decoder_get_length_in_pcm_frames(&r->decoder, &length) == MA_SUCCESS) r->info.frames = length;
            if (sane(&r->info)) { *info = r->info; return r; }
            ma_decoder_uninit(&r->decoder);
        }
        fclose(r->file); r->file = NULL;
    }
    r->platform = fl_audio_file_open(path, &r->info.sample_rate, &r->info.channels, &r->info.frames);
    if (r->platform && sane(&r->info) && (!rate || rate == r->info.sample_rate) && (!channels || channels == r->info.channels)) { *info = r->info; return r; }
    fl_audio_file_close(r->platform);
    free(r);
    return NULL;
}

Fl_AudioReader *fl_audio_open(const char *path, Fl_AudioInfo *info)
{
    return open_format(path, info, 0, 0);
}

Fl_AudioReader *fl_audio_open_resampled(const char *path, Fl_AudioInfo *info, uint32_t rate, uint32_t channels)
{
    if (rate < 1000 || rate > 768000 || !channels || channels > 32) return NULL;
    return open_format(path, info, rate, channels);
}

uint64_t fl_audio_read(Fl_AudioReader *r, float *out, uint64_t frames)
{
    if (!r || !out || !frames) return 0;
    if (r->platform) return fl_audio_file_read(r->platform, out, frames);
    ma_uint64 read = 0;
    ma_result result = ma_decoder_read_pcm_frames(&r->decoder, out, frames, &read);
    return result == MA_SUCCESS || result == MA_AT_END ? read : 0;
}

bool fl_audio_seek(Fl_AudioReader *r, uint64_t frame)
{
    if (!r) return false;
    if (r->platform) return fl_audio_file_seek(r->platform, frame);
    return ma_decoder_seek_to_pcm_frame(&r->decoder, frame) == MA_SUCCESS;
}

void fl_audio_close(Fl_AudioReader *r)
{
    if (!r) return;
    if (r->platform) fl_audio_file_close(r->platform);
    else { ma_decoder_uninit(&r->decoder); fclose(r->file); }
    free(r);
}

bool fl_audio_probe(const char *path, Fl_AudioInfo *info)
{
    Fl_AudioInfo found;
    Fl_AudioReader *r = fl_audio_open(path, &found);
    if (!r) return false;
    float *block = malloc((size_t)BLOCK_FRAMES * found.channels * sizeof(float));
    uint64_t first = block ? fl_audio_read(r, block, BLOCK_FRAMES) : 0;
    bool ok = first > 0;
    if (ok && !found.frames) {
        found.frames = first;
        for (uint64_t n; (n = fl_audio_read(r, block, BLOCK_FRAMES)) > 0; ) found.frames += n;
    }
    free(block); fl_audio_close(r);
    if (ok && info) *info = found;
    return ok;
}

bool fl_audio_waveform(const char *path, float *peaks, size_t bins)
{
    if (!peaks || !bins) return false;
    Fl_AudioInfo info;
    Fl_AudioReader *r = fl_audio_open(path, &info);
    if (!r) return false;
    float *block = malloc((size_t)BLOCK_FRAMES * info.channels * sizeof(float));
    if (!block) { fl_audio_close(r); return false; }
    for (size_t i = 0; i < bins; ++i) { peaks[2 * i] = 0; peaks[2 * i + 1] = 0; }
    uint64_t total = info.frames, frame = 0;
    if (!total) {
        for (uint64_t n; (n = fl_audio_read(r, block, BLOCK_FRAMES)) > 0; ) total += n;
        if (!fl_audio_seek(r, 0)) { free(block); fl_audio_close(r); return false; }
    }
    for (uint64_t n; total && (n = fl_audio_read(r, block, BLOCK_FRAMES)) > 0; frame += n)
        for (uint64_t f = 0; f < n; ++f) {
            uint64_t at = frame + f;
            size_t bin = (size_t)((at >= total ? total - 1 : at) * bins / total);
            for (uint32_t c = 0; c < info.channels; ++c) {
                float v = block[f * info.channels + c];
                if (v < peaks[2 * bin]) peaks[2 * bin] = v < -1 ? -1 : v;
                if (v > peaks[2 * bin + 1]) peaks[2 * bin + 1] = v > 1 ? 1 : v;
            }
        }
    free(block); fl_audio_close(r);
    return total > 0;
}

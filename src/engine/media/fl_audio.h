#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Decoded stream description. */
typedef struct Fl_AudioInfo {
    uint32_t sample_rate, channels;
    uint64_t frames;                /* total PCM frames; zero when the container does not say */
} Fl_AudioInfo;

/*
 * Streaming decoder producing interleaved 32-bit float at the file's own rate
 * and channel count: miniaudio (WAV, FLAC, MP3, Ogg Vorbis) first, then the
 * platform decoder (AAC/ALAC on macOS). Paths are UTF-8. A reader is used by
 * one thread at a time.
 */
typedef struct Fl_AudioReader Fl_AudioReader;

/* Open path and describe it; NULL when no decoder accepts the file. */
Fl_AudioReader *fl_audio_open(const char *path, Fl_AudioInfo *info);
/* Decode up to frames frames into out (frames * channels floats); returns frames produced, 0 at the end. */
uint64_t fl_audio_read(Fl_AudioReader *reader, float *out, uint64_t frames);
/* Move to frame; false on failure. */
bool fl_audio_seek(Fl_AudioReader *reader, uint64_t frame);
/* Release the reader. NULL is safe. */
void fl_audio_close(Fl_AudioReader *reader);
/* Validate and describe an audio file by decoding its first block; when the container omits the
   length, the whole stream is decoded to count frames. Blocking; call from a worker. */
bool fl_audio_probe(const char *path, Fl_AudioInfo *info);
/*
 * Decode the whole file into a peak envelope: peaks[2*i] / peaks[2*i+1] are
 * the minimum / maximum sample (all channels) of bin i, in [-1, 1].
 */
bool fl_audio_waveform(const char *path, float *peaks, size_t bins);

#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Platform audio-file decoder used when the portable decoders cannot read a
 * format (AAC/ALAC in MP4/M4A). AudioToolbox on macOS; unavailable elsewhere.
 * Output is interleaved 32-bit float at the file's own rate and channel count.
 */
typedef struct Fl_AudioFile Fl_AudioFile;

/* Open path; rate/channels/frames receive the stream description (frames 0 when unknown). NULL when unsupported. */
Fl_AudioFile *fl_audio_file_open(const char *path, uint32_t *rate, uint32_t *channels, uint64_t *frames);
/* Decode up to frames frames into out; returns frames produced, 0 at the end or on error. */
uint64_t fl_audio_file_read(Fl_AudioFile *file, float *out, uint64_t frames);
/* Move the read position to frame; false on failure. */
bool fl_audio_file_seek(Fl_AudioFile *file, uint64_t frame);
/* Close the file. NULL is safe. */
void fl_audio_file_close(Fl_AudioFile *file);

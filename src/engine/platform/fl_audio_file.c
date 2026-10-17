#include "fl_audio_file.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>

struct Fl_AudioFile { ExtAudioFileRef file; uint32_t channels; };

Fl_AudioFile *fl_audio_file_open(const char *path, uint32_t *rate, uint32_t *channels, uint64_t *frames)
{
    if (!path || !rate || !channels || !frames) return NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)path, (CFIndex)strlen(path), false);
    if (!url) return NULL;
    ExtAudioFileRef file = NULL;
    OSStatus status = ExtAudioFileOpenURL(url, &file);
    CFRelease(url);
    if (status != noErr || !file) return NULL;
    AudioStreamBasicDescription source = {0};
    UInt32 size = sizeof(source);
    SInt64 length = 0;
    UInt32 length_size = sizeof(length);
    if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &source) != noErr ||
        source.mSampleRate < 1 || source.mSampleRate > 768000 || !source.mChannelsPerFrame || source.mChannelsPerFrame > 64) {
        ExtAudioFileDispose(file); return NULL;
    }
    AudioStreamBasicDescription client = {.mSampleRate = source.mSampleRate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, .mBitsPerChannel = 32,
        .mChannelsPerFrame = source.mChannelsPerFrame, .mFramesPerPacket = 1,
        .mBytesPerFrame = 4 * source.mChannelsPerFrame, .mBytesPerPacket = 4 * source.mChannelsPerFrame};
    if (ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client) != noErr) {
        ExtAudioFileDispose(file); return NULL;
    }
    if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileLengthFrames, &length_size, &length) != noErr || length < 0) length = 0;
    Fl_AudioFile *result = malloc(sizeof(*result));
    if (!result) { ExtAudioFileDispose(file); return NULL; }
    result->file = file; result->channels = source.mChannelsPerFrame;
    *rate = (uint32_t)(source.mSampleRate + 0.5); *channels = source.mChannelsPerFrame; *frames = (uint64_t)length;
    return result;
}

uint64_t fl_audio_file_read(Fl_AudioFile *f, float *out, uint64_t frames)
{
    if (!f || !out || !frames) return 0;
    uint64_t total = 0;
    while (total < frames) {
        uint64_t want = frames - total;
        if (want > UINT32_MAX / (4u * f->channels)) want = UINT32_MAX / (4u * f->channels);
        UInt32 count = (UInt32)want;
        AudioBufferList list = {.mNumberBuffers = 1, .mBuffers = {{.mNumberChannels = f->channels,
            .mDataByteSize = count * 4 * f->channels, .mData = out + total * f->channels}}};
        if (ExtAudioFileRead(f->file, &count, &list) != noErr || !count) break;
        total += count;
    }
    return total;
}

bool fl_audio_file_seek(Fl_AudioFile *f, uint64_t frame)
{
    return f && frame <= INT64_MAX && ExtAudioFileSeek(f->file, (SInt64)frame) == noErr;
}

void fl_audio_file_close(Fl_AudioFile *f)
{
    if (!f) return;
    ExtAudioFileDispose(f->file);
    free(f);
}

#else

Fl_AudioFile *fl_audio_file_open(const char *path, uint32_t *rate, uint32_t *channels, uint64_t *frames)
{
    (void)path; (void)rate; (void)channels; (void)frames; return NULL;
}
uint64_t fl_audio_file_read(Fl_AudioFile *file, float *out, uint64_t frames) { (void)file; (void)out; (void)frames; return 0; }
bool fl_audio_file_seek(Fl_AudioFile *file, uint64_t frame) { (void)file; (void)frame; return false; }
void fl_audio_file_close(Fl_AudioFile *file) { (void)file; }

#endif

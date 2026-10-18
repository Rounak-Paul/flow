#include "fl_video_cache.h"
#include "core/fl_log.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include "platform/fl_process.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "video"

typedef struct FrameCount { size_t count; } FrameCount;

static bool count_entry(const char *name, bool is_dir, void *data)
{
    FrameCount *frames = data;
    size_t n = strlen(name);
    if (!is_dir && n > 4 && !strcmp(name + n - 4, ".jpg")) ++frames->count;
    return true;
}

/* Cache folder of an asset, keyed by content hash so identical videos share frames. */
static bool cache_dir(const Fl_Project *project, const Fl_Asset *asset, const char *suffix, char out[FL_PATH_MAX])
{
    char hex[65], leaf[128], base[FL_PATH_MAX];
    for (size_t i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", asset->hash[i]);
    snprintf(leaf, sizeof(leaf), ".flow/cache/video/%s%s", hex, suffix);
    if (!fl_fs_join(base, sizeof(base), fl_project_directory(project), leaf)) return false;
    strcpy(out, base);
    return true;
}

/* Number of frames recorded in the completion marker, or zero when the cache is absent or unfinished. */
static size_t frames_ready(const char *dir)
{
    char marker[FL_PATH_MAX];
    if (!fl_fs_join(marker, sizeof(marker), dir, "frames.txt")) return 0;
    size_t size = 0;
    char *text = fl_fs_read_file(marker, 64, &size);
    size_t count = text ? (size_t)strtoull(text, NULL, 10) : 0;
    free(text);
    return count;
}

bool fl_video_cache_prepare(const Fl_Project *project, const Fl_Asset *asset)
{
    char dir[FL_PATH_MAX], part[FL_PATH_MAX], source[FL_PATH_MAX];
    if (!project || !asset || asset->kind != FL_ASSET_VIDEO || !cache_dir(project, asset, "", dir) || !cache_dir(project, asset, ".part", part)) return false;
    if (frames_ready(dir)) return true;
    const char *ffmpeg = fl_ffmpeg_path();
    if (!ffmpeg) {FL_LOG_ERROR(LOG_TAG, "ffmpeg was not found; install it or set FLOW_FFMPEG to use video"); return false;}
    if (!fl_project_asset_path(project, asset, source, sizeof(source))) return false;
    fl_fs_remove(part);
    char pattern[FL_PATH_MAX], command[4 * FL_PATH_MAX], filter[96];
    snprintf(filter, sizeof(filter), "fps=%u,scale='min(1920,iw)':-2", FL_VIDEO_CACHE_FPS);
    if (!fl_fs_mkdir_p(part) || !fl_fs_join(pattern, sizeof(pattern), part, "%06d.jpg")) return false;
    command[0] = 0;
    bool built = fl_command_append(command, sizeof(command), ffmpeg) && fl_command_append(command, sizeof(command), "-v") &&
        fl_command_append(command, sizeof(command), "error") && fl_command_append(command, sizeof(command), "-y") &&
        fl_command_append(command, sizeof(command), "-i") && fl_command_append(command, sizeof(command), source) &&
        fl_command_append(command, sizeof(command), "-an") && fl_command_append(command, sizeof(command), "-vf") &&
        fl_command_append(command, sizeof(command), filter) && fl_command_append(command, sizeof(command), "-q:v") &&
        fl_command_append(command, sizeof(command), "2") && fl_command_append(command, sizeof(command), pattern);
    FL_LOG_INFO(LOG_TAG, "extracting frames of %s", asset->name);
    bool ok = built && fl_command_run(command);
    FrameCount frames = {0};
    if (ok) fl_fs_list_dir(part, count_entry, &frames);
    char marker[FL_PATH_MAX], text[32];
    snprintf(text, sizeof(text), "%zu", frames.count);
    ok = ok && frames.count && fl_fs_join(marker, sizeof(marker), part, "frames.txt") && fl_fs_write_atomic(marker, text, strlen(text));
    fl_fs_remove(dir);
    ok = ok && fl_fs_rename(part, dir);
    if (!ok) FL_LOG_ERROR(LOG_TAG, "could not extract frames of %s", asset->name);
    return ok;
}

bool fl_video_cache_frame(const Fl_Project *project, const Fl_Asset *asset, double seconds, char *out, size_t capacity)
{
    char dir[FL_PATH_MAX], leaf[32];
    if (!fl_video_cache_prepare(project, asset) || !cache_dir(project, asset, "", dir)) return false;
    size_t frames = frames_ready(dir);
    if (!frames) return false;
    double index = isfinite(seconds) ? floor(seconds * FL_VIDEO_CACHE_FPS + 1e-6) : 0;
    index = fmin(fmax(index, 0), (double)(frames - 1));
    snprintf(leaf, sizeof(leaf), "%06zu.jpg", (size_t)index + 1);
    return fl_fs_join(out, capacity, dir, leaf);
}

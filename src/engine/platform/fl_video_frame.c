#include "fl_video_frame.h"
#include <stddef.h>

#if !defined(__APPLE__)
unsigned char *fl_video_frame(const char *path, uint32_t max_side, uint32_t *width, uint32_t *height,
                              uint32_t *video_width, uint32_t *video_height, double *seconds)
{
    (void)path; (void)max_side; (void)width; (void)height; (void)video_width; (void)video_height; (void)seconds;
    return NULL;
}
#endif

#if !defined(__APPLE__)
unsigned char *fl_video_frame_at(const char *path, double seconds, uint32_t max_side,
                                 uint32_t *width, uint32_t *height, const atomic_bool *cancel)
{
    (void)path; (void)seconds; (void)max_side; (void)width; (void)height; (void)cancel; return NULL;
}
#endif

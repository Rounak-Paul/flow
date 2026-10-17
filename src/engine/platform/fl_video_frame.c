#include "fl_video_frame.h"
#include <stddef.h>

#if !defined(__APPLE__)
unsigned char *fl_video_frame(const char *path, uint32_t max_side, uint32_t *width, uint32_t *height,
                              uint32_t *video_width, uint32_t *video_height)
{
    (void)path; (void)max_side; (void)width; (void)height; (void)video_width; (void)video_height;
    return NULL;
}
#endif

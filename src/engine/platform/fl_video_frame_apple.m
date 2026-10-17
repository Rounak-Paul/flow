#include "fl_video_frame.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Wait for an asynchronous AVFoundation callback; completion always signals. */
static bool wait_for(dispatch_semaphore_t done)
{
    return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * (int64_t)NSEC_PER_SEC)) == 0;
}

/* Draw a CGImage into a tightly packed sRGB RGBA8 buffer of w x h. */
static unsigned char *render(CGImageRef image, uint32_t w, uint32_t h)
{
    unsigned char *pixels = calloc((size_t)w * h, 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = pixels && space ? CGBitmapContextCreate(pixels, w, h, 8, (size_t)w * 4, space,
        (CGBitmapInfo)kCGImageAlphaPremultipliedLast) : NULL;
    if (context) {
        CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
        CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
        CGContextRelease(context);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            unsigned a = pixels[i * 4 + 3];
            if (a && a < 255) for (unsigned c = 0; c < 3; ++c) pixels[i * 4 + c] = (unsigned char)((pixels[i * 4 + c] * 255u + a / 2) / a);
        }
    } else { free(pixels); pixels = NULL; }
    if (space) CGColorSpaceRelease(space);
    return pixels;
}

unsigned char *fl_video_frame(const char *path, uint32_t max_side, uint32_t *width, uint32_t *height,
                              uint32_t *video_width, uint32_t *video_height, double *out_seconds)
{
    if (!path || !max_side || !width || !height) return NULL;
    if (@available(macOS 13.0, *)) {
        @autoreleasepool {
            NSString *file = [NSString stringWithUTF8String:path];
            if (!file) return NULL;
            AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:file] options:nil];
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [asset loadValuesAsynchronouslyForKeys:@[@"duration"] completionHandler:^{ dispatch_semaphore_signal(done); }];
            if (!wait_for(done)) return NULL;
            __block NSArray<AVAssetTrack *> *tracks = nil;
            [asset loadTracksWithMediaType:AVMediaTypeVideo completionHandler:^(NSArray<AVAssetTrack *> *found, NSError *error) {
                (void)error; tracks = found; dispatch_semaphore_signal(done);
            }];
            if (!wait_for(done) || tracks.count == 0) return NULL;
            AVAssetTrack *track = tracks[0];
            [track loadValuesAsynchronouslyForKeys:@[@"naturalSize", @"preferredTransform"] completionHandler:^{ dispatch_semaphore_signal(done); }];
            if (!wait_for(done) || [track statusOfValueForKey:@"naturalSize" error:nil] != AVKeyValueStatusLoaded) return NULL;
            CGSize natural = track.naturalSize;
            CGAffineTransform orientation = [track statusOfValueForKey:@"preferredTransform" error:nil] == AVKeyValueStatusLoaded
                ? track.preferredTransform : CGAffineTransformIdentity;
            CMTime duration = [asset statusOfValueForKey:@"duration" error:nil] == AVKeyValueStatusLoaded ? asset.duration : kCMTimeZero;
            CGSize shown = CGRectApplyAffineTransform(CGRectMake(0, 0, natural.width, natural.height), orientation).size;
            double vw = fabs(shown.width), vh = fabs(shown.height);
            if (vw < 1 || vh < 1) return NULL;
            if (video_width) *video_width = (uint32_t)lround(vw);
            if (video_height) *video_height = (uint32_t)lround(vh);
            double seconds = CMTIME_IS_NUMERIC(duration) ? CMTimeGetSeconds(duration) : 0;
            if (out_seconds) *out_seconds = seconds > 0 && seconds < 1e9 ? seconds : 0;
            CMTime at = CMTimeMakeWithSeconds(fmin(1.0, fmax(0.0, seconds * 0.1)), 600);
            AVAssetImageGenerator *generator = [AVAssetImageGenerator assetImageGeneratorWithAsset:asset];
            generator.appliesPreferredTrackTransform = YES;
            generator.maximumSize = CGSizeMake(max_side, max_side);
            generator.requestedTimeToleranceBefore = kCMTimePositiveInfinity;
            generator.requestedTimeToleranceAfter = kCMTimePositiveInfinity;
            __block CGImageRef frame = NULL;
            [generator generateCGImageAsynchronouslyForTime:at completionHandler:^(CGImageRef image, CMTime actual, NSError *error) {
                (void)actual; (void)error;
                if (image) frame = CGImageRetain(image);
                dispatch_semaphore_signal(done);
            }];
            bool finished = wait_for(done);
            if (!finished || !frame) { if (finished && frame) CGImageRelease(frame); return NULL; }
            size_t fw = CGImageGetWidth(frame), fh = CGImageGetHeight(frame);
            double scale = fmin(1.0, (double)max_side / (double)(fw > fh ? fw : fh));
            uint32_t w = (uint32_t)fmax(1, lround(fw * scale)), h = (uint32_t)fmax(1, lround(fh * scale));
            unsigned char *pixels = fw && fh ? render(frame, w, h) : NULL;
            CGImageRelease(frame);
            if (pixels) { *width = w; *height = h; }
            return pixels;
        }
    }
    return NULL;
}

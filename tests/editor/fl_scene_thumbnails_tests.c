#include "flow_test.h"
#include "../../src/editor/fl_scene_thumbnails.c"

/* Thumbnail pixels track the shared camera crop while the underlying entity remains unchanged. */
int main(void)
{
    unsigned char transform[FL_POSE_BYTES], bounds_bytes[FL_BOUNDS_BYTES];
    Fl_Pose pose={.x=900,.y=500,.sx=1,.sy=1,.sz=1};
    Fl_Bounds bounds={320,180,0};
    fl_pose_encode(&pose,transform); fl_bounds_encode(&bounds,bounds_bytes);
    const unsigned char color[4]={240,40,80,255};
    Fl_Component components[]={{.type=FL_COMPONENT_POSE, .version=1, .size=sizeof(transform), .data=transform},
        {.type=FL_COMPONENT_BOUNDS, .version=1, .size=sizeof(bounds_bytes), .data=bounds_bytes},
        {.type=FL_COMPONENT_RECTANGLE, .version=1, .size=sizeof(color), .data=color}};
    Fl_Entity entity={.id=1,.name="Rectangle",.visible=true,.components=components,.component_count=3};
    Fl_Scene scene={.id=1,.revision=1,.width=1920,.height=1080,.duration=600000,
        .aspect_width=9,.aspect_height=16,.entities=&entity,.entity_count=1};
    unsigned char *centered=rasterize(&scene);CHECK(centered);
    scene.camera_x=1000;scene.revision++;
    unsigned char *panned=rasterize(&scene);CHECK(panned);
    if (centered && panned) {
        CHECK(!memcmp(centered, panned, (size_t)FL_STYLE_SCENE_THUMB_W * FL_STYLE_SCENE_THUMB_H * 4));
        CHECK(entity.components[0].data==transform);
    }
    scene.camera_x=0;scene.revision++;
    unsigned char *restored=rasterize(&scene);CHECK(restored);
    if (centered && restored) CHECK(!memcmp(centered,restored,(size_t)FL_STYLE_SCENE_THUMB_W*FL_STYLE_SCENE_THUMB_H*4));
    free(centered);free(panned);free(restored);
    return FLOW_TEST_RESULT("fl_scene_thumbnails_tests");
}

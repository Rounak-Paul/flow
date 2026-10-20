#include "flow_test.h"
#include "../../src/editor/fl_scene_thumbnails.c"

/* Thumbnail pixels track the shared camera crop while the underlying entity remains unchanged. */
int main(void)
{
    unsigned char transform[56];
    Fl_Transform pose={900,500,320,180,0,1,1};
    fl_transform_encode(&pose,transform);
    const unsigned char color[4]={240,40,80,255};
    Fl_Component components[]={{.type=FL_COMPONENT_TRANSFORM, .version=1, .size=sizeof(transform), .data=transform},
        {.type=FL_COMPONENT_RECTANGLE, .version=1, .size=sizeof(color), .data=color}};
    Fl_Entity entity={.id=1,.name="Rectangle",.visible=true,.components=components,.component_count=2};
    Fl_Scene scene={.id=1,.revision=1,.width=1920,.height=1080,.duration=600000,
        .aspect_width=9,.aspect_height=16,.entities=&entity,.entity_count=1};
    unsigned char *centered=rasterize(&scene);CHECK(centered);
    scene.camera_x=1000;scene.revision++;
    unsigned char *panned=rasterize(&scene);CHECK(panned);
    if (centered && panned) {
        size_t pixel=((size_t)FL_STYLE_SCENE_THUMB_H/2*FL_STYLE_SCENE_THUMB_W+FL_STYLE_SCENE_THUMB_W/2)*4;
        CHECK(centered[pixel]>panned[pixel]+100);
        CHECK(entity.components[0].data==transform);
    }
    scene.camera_x=0;scene.revision++;
    unsigned char *restored=rasterize(&scene);CHECK(restored);
    if (centered && restored) CHECK(!memcmp(centered,restored,(size_t)FL_STYLE_SCENE_THUMB_W*FL_STYLE_SCENE_THUMB_H*4));
    free(centered);free(panned);free(restored);
    return FLOW_TEST_RESULT("fl_scene_thumbnails_tests");
}

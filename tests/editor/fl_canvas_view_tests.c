#include "flow_test.h"
#include "fl_canvas_view.h"
#include <math.h>

/* Verify camera centering, pan displacement, Retina-equivalent mapping and invalid-size rejection. */
int main(void)
{
    Fl_Scene scene={.width=1920,.height=1080,.aspect_width=9,.aspect_height=16};
    Fl_SceneFrame target=fl_scene_frame(&scene);
    Fl_CanvasMapping a=fl_canvas_mapping(target,800,600,14);
    CHECK(a.scale>0);
    CHECK(fabs(a.world.x+400/a.scale-960)<1e-8);
    CHECK(fabs(a.world.y+300/a.scale-540)<1e-8);
    Fl_SceneFrame shifted=target; shifted.x-=100; shifted.y+=200;
    Fl_CanvasMapping pan=fl_canvas_mapping(shifted,800,600,14);
    CHECK(fabs(pan.world.x-a.world.x+100)<1e-8);
    CHECK(fabs(pan.world.y-a.world.y-200)<1e-8);
    CHECK(pan.scale==a.scale);
    CHECK(fabs((pan.target.x-pan.world.x)*pan.scale-(a.target.x-a.world.x)*a.scale)<1e-8);
    CHECK(fabs((pan.target.y-pan.world.y)*pan.scale-(a.target.y-a.world.y)*a.scale)<1e-8);
    CHECK(fabs((pan.target.x+pan.target.width*.5-pan.world.x)*pan.scale-400)<1e-8);
    CHECK(fabs((pan.target.y+pan.target.height*.5-pan.world.y)*pan.scale-300)<1e-8);
    CHECK(pan.target.width==target.width && pan.target.height==target.height);
    Fl_CanvasMapping retina=fl_canvas_mapping(target,1600,1200,28);
    CHECK(retina.world.width==a.world.width && retina.world.height==a.world.height);
    CHECK(retina.scale==a.scale*2);
    CHECK(fl_canvas_mapping(target,20,20,14).scale==0);
    CHECK(fl_canvas_mapping((Fl_SceneFrame){NAN,0,100,100},800,600,14).scale==0);
    CHECK(fl_canvas_mapping(target,INFINITY,600,14).scale==0);
    CHECK(fl_canvas_mapping((Fl_SceneFrame){0},800,600,14).scale==0);
    return FLOW_TEST_RESULT("fl_canvas_view_tests");
}

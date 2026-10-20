#include "flow_test.h"
#include "time/fl_time_mapping.h"
#include "project/fl_project.h"
#include <math.h>
#include <stdlib.h>

/* Verify mapping math and boundaries independently of transport or Lua execution. */
int main(void)
{
    const Fl_Tick second=FL_TICKS_PER_SECOND;
    Fl_Beat beats[]={{1,"Before",2*FL_TICKS_PER_SECOND,false},{2,"Effect",3*FL_TICKS_PER_SECOND,false}};
    Fl_Scene scene={.duration=5*FL_TICKS_PER_SECOND,.beats=beats,.beat_count=2};
    Fl_TimeMapping map=fl_time_mapping_default(); map.explicit_range=true; map.start=(Fl_TimeAnchor){.kind=FL_ANCHOR_BEAT,.beat=2,.end=false,.offset=0};
    map.end=(Fl_TimeAnchor){.kind=FL_ANCHOR_BEAT,.beat=2,.end=true,.offset=0}; map.duration=second;
    Fl_TimeSample sample;
    CHECK(fl_time_mapping_sample(&scene,&map,second,&sample) && !sample.active && sample.phase==FL_TIME_BEFORE);
    CHECK(fl_time_mapping_sample(&scene,&map,3*second+second/2,&sample) && sample.tick==second/2 && sample.progress==0.5 && sample.active);
    CHECK(fl_time_mapping_sample(&scene,&map,5*second,&sample) && sample.tick==second && sample.progress==1 && sample.active && sample.phase==FL_TIME_AFTER);
    map.after=FL_TIME_INACTIVE; CHECK(fl_time_mapping_sample(&scene,&map,5*second,&sample) && !sample.active);
    map.after=FL_TIME_HOLD; beats[1].duration=6*FL_TICKS_PER_SECOND;
    CHECK(fl_time_mapping_sample(&scene,&map,5*second,&sample) && sample.progress==0.5);
    map.mode=FL_RETIME_FIXED;
    CHECK(fl_time_mapping_sample(&scene,&map,2*second+second/2,&sample) && sample.tick==second/2);
    CHECK(fl_time_mapping_sample(&scene,&map,5*second,&sample) && sample.tick==second);
    map.loop=true;
    CHECK(fl_time_mapping_sample(&scene,&map,4*second+second/2,&sample) && sample.tick==second/2 && sample.cycle==2);
    CHECK(fl_time_mapping_sample(&scene,&map,8*second,&sample) && sample.tick==second && sample.cycle==5);
    map.start.beat=999; CHECK(!fl_time_mapping_sample(&scene,&map,second,&sample)); map.start.beat=2;
    map.rate=NAN; CHECK(!fl_time_mapping_sample(&scene,&map,second,&sample)); map.rate=1;
    map.end=(Fl_TimeAnchor){.kind=FL_ANCHOR_ABSOLUTE,.beat=0,.end=false,.offset=second}; CHECK(!fl_time_mapping_sample(&scene,&map,second,&sample));
    Fl_TimeAnchor normalized={.kind=FL_ANCHOR_BEAT_POSITION,.beat=2,.position=0.25,.offset=second/10};
    Fl_TimeAnchor fixed={.kind=FL_ANCHOR_BEAT,.beat=2,.offset=second};
    Fl_Tick point;
    CHECK(fl_anchor_sample(&scene,&normalized,&point) && point==3*second+second/2+second/10);
    beats[1].duration=3*FL_TICKS_PER_SECOND;
    CHECK(fl_anchor_sample(&scene,&normalized,&point) && point==2*second+3*second/4+second/10);
    CHECK(fl_anchor_sample(&scene,&fixed,&point) && point==3*second);
    normalized.position=NAN; CHECK(!fl_anchor_sample(&scene,&normalized,&point));
    normalized.position=1.01; CHECK(!fl_anchor_sample(&scene,&normalized,&point));
    normalized.position=0.25; normalized.beat=999; CHECK(!fl_anchor_sample(&scene,&normalized,&point));
    Fl_Binary bytes={0}; normalized.beat=2; fl_time_anchor_write(&normalized,&bytes);
    Fl_TimeAnchor decoded={0}; CHECK(fl_time_anchor_read(&decoded,&bytes) && decoded.position==0.25 && decoded.offset==second/10);
    free(bytes.data);
    map=fl_time_mapping_default(); map.duration=INT64_MAX;
    CHECK(fl_time_mapping_sample(&scene,&map,INT64_MAX,&sample) && sample.tick==INT64_MAX);
    return FLOW_TEST_RESULT("fl_time_mapping_tests");
}

#include "flow_test.h"
#include "components/fl_sampled_animation.h"
#include "schema/fl_properties.h"
#include <math.h>
#include <string.h>

/* Verify native clip interpolation, held text, document round trips and malformed input rejection. */
int main(void)
{
    Fl_SampledKey keys[3]={0};
    for (unsigned i=0;i<3;++i) {
        keys[i].tick=(Fl_Tick)i*100;
        for (int p=0;p<FL_PROP_COUNT;++p) keys[i].values[p][0]=fl_property_definition((Fl_AnimProperty)p)->minimum;
        keys[i].values[FL_PROP_X][0]=(double)i*20;
        keys[i].text=i==2 ? "changed" : "hello";
        keys[i].text_size=strlen(keys[i].text);
    }
    Fl_Component clip={0}; CHECK(fl_sampled_animation_encode(keys,3,true,&clip));
    CHECK(fl_sampled_animation_valid(&clip));
    Fl_SampledKey pose={0}; bool text=false;
    CHECK(fl_sampled_animation_sample(&clip,50,&pose,&text));
    CHECK(text && pose.values[FL_PROP_X][0]==10 && pose.text_size==5 && !memcmp(pose.text,"hello",5));
    CHECK(fl_sampled_animation_sample(&clip,-1,&pose,NULL) && pose.values[FL_PROP_X][0]==0);
    CHECK(fl_sampled_animation_sample(&clip,200,&pose,NULL) && pose.text_size==7);
    CHECK(fl_sampled_animation_sample(&clip,INT64_MAX,&pose,NULL) && pose.values[FL_PROP_X][0]==40);
    Fl_Json context={0}; cJSON *json=fl_sampled_animation_write(&context,&clip); CHECK(json);
    Fl_Component decoded={0}; CHECK(fl_sampled_animation_read(&context,json,&decoded));
    CHECK(decoded.size==clip.size && !memcmp(decoded.data,clip.data,clip.size));
    cJSON_Delete(json); fl_component_clear(&decoded);
    unsigned char *bytes=(unsigned char *)clip.data;
    unsigned char saved=bytes[16]; bytes[16]=255;
    CHECK(!fl_sampled_animation_valid(&clip)); bytes[16]=saved;
    size_t size=clip.size; clip.size=16; CHECK(!fl_sampled_animation_valid(&clip)); clip.size=size;
    fl_component_clear(&clip);
    keys[1].tick=0; CHECK(!fl_sampled_animation_encode(keys,3,true,&clip)); keys[1].tick=100;
    keys[1].values[FL_PROP_X][0]=NAN; CHECK(!fl_sampled_animation_encode(keys,3,true,&clip)); keys[1].values[FL_PROP_X][0]=20;
    keys[1].text="\xc0\x80"; keys[1].text_size=2; CHECK(!fl_sampled_animation_encode(keys,3,true,&clip));
    CHECK(fl_sampled_animation_encode(keys,3,false,&clip)); CHECK(fl_sampled_animation_valid(&clip));
    CHECK(fl_sampled_animation_sample(&clip,50,&pose,&text) && !text);
    fl_component_clear(&clip);
    return FLOW_TEST_RESULT("fl_sampled_animation_tests");
}

#include "flow_test.h"
#include "schema/fl_widgets.h"
#include "components/fl_style.h"
#include "components/fl_video_clip.h"
#include "doc/fl_scene_geometry.h"
#include "platform/fl_fs.h"
#include "platform/fl_paths.h"
#include <math.h>
#include <string.h>

/* Replace a local transform through the same atomic operation used by viewport gestures. */
static bool transform(Fl_Project *project,Fl_Id scene,Fl_Id entity,Fl_Pose pose,Fl_Bounds bounds)
{
    Fl_TransformEdit edit={entity,pose,bounds};
    return fl_project_apply(project,&(Fl_Op){.kind=FL_OP_TRANSFORMS,.scene=scene,.transforms=&edit,.transform_count=1},NULL);
}
/* Verify every registered primitive, schema validation and persisted representation. */
int main(int argc,char **argv)
{
    CHECK(argc==2);if (argc!=2) return 1;
    char path[FL_PATH_MAX];CHECK(fl_fs_mkdir_p(argv[1]));
    CHECK(fl_fs_join(path,sizeof(path),argv[1],"project.flow.json"));fl_fs_remove(path);
    Fl_Project *p=fl_project_create(argv[1],"Widgets",NULL);CHECK(p);if (!p) return 1;
    Fl_Id scene=fl_project_scene_at(p,0)->id,ids[FL_WIDGET_COUNT]={0};
    for (int i=0;i<FL_WIDGET_COUNT;++i) {
        const Fl_WidgetDef *def=fl_widget_definition((Fl_WidgetKind)i);CHECK(def);
        if (i==FL_WIDGET_IMAGE||i==FL_WIDGET_VIDEO) {CHECK(!fl_widget_create(p,scene,0,(Fl_WidgetKind)i,0,0,&ids[i]));continue;}
        CHECK(fl_widget_create(p,scene,0,(Fl_WidgetKind)i,i*600,20,&ids[i]));
        const Fl_Entity *e=fl_scene_entity(fl_project_scene(p,scene),ids[i]);
        CHECK(fl_widget_for_entity(e)==def);
        Fl_Pose t;CHECK(fl_pose_decode(fl_entity_component(e,FL_COMPONENT_POSE),&t));CHECK(t.x==i*600+def->bounds.width*0.5);
    }
    CHECK(!fl_widget_definition((Fl_WidgetKind)-1));CHECK(!fl_widget_create(p,scene,0,FL_WIDGET_RECTANGLE,NAN,0,NULL));
    const Fl_Scene *s=fl_project_scene(p,scene);size_t count=s->entity_count;
    unsigned char invalid_geometry=255;
    Fl_Component invalid={.type = FL_COMPONENT_GEOMETRY, .version = 1, .size = 1, .data = &invalid_geometry};
    CHECK(!fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="Invalid",.components=&invalid,.component_count=1},NULL));
    CHECK(fl_project_scene(p,scene)->entity_count==count);
    Fl_Component duplicates[2]={{.type=FL_COMPONENT_TEXT, .version=1, .size=0, .data=NULL},{.type=FL_COMPONENT_TEXT, .version=1, .size=0, .data=NULL}};
    CHECK(!fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ENTITY_ADD,.scene=scene,.name="Invalid",.components=duplicates,.component_count=2},NULL));
    Fl_Appearance a=fl_widget_definition(FL_WIDGET_ELLIPSE)->appearance,decoded;
    a.stroke_width=12;a.fill[3]=128;unsigned char style[24];fl_appearance_encode(&a,style);
    Fl_Component appearance={.type = FL_COMPONENT_APPEARANCE, .version = 1, .size = 24, .data = style};CHECK(fl_appearance_decode(&appearance,&decoded));CHECK(decoded.stroke_width==12&&decoded.fill[3]==128);
    unsigned char opaque[]={1,2,3};Fl_Component updates[2]={appearance,{.type=999, .version=9, .size=3, .data=opaque}};
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ENTITY_CONFIGURE,.scene=scene,.entity=ids[FL_WIDGET_ELLIPSE],.name="Circle",.visible=true,.components=updates,.component_count=2},NULL));
    a.stroke_width=-1;fl_appearance_encode(&a,style);
    CHECK(!fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ENTITY_CONFIGURE,.scene=scene,.entity=ids[FL_WIDGET_ELLIPSE],.name="Must not rename",.visible=false,.components=&appearance,.component_count=1},NULL));
    const Fl_Entity *ellipse=fl_scene_entity(fl_project_scene(p,scene),ids[FL_WIDGET_ELLIPSE]);CHECK(!strcmp(ellipse->name,"Circle")&&ellipse->visible);
    CHECK(fl_widget_appearance(ellipse).stroke_width==12);
    Fl_SceneGeometry geometry={0};CHECK(fl_scene_geometry_update(&geometry,fl_project_scene(p,scene),NULL));
    CHECK(fl_scene_geometry_pick(&geometry,fl_project_scene(p,scene),1201,21,0)==0);
    CHECK(fl_scene_geometry_pick(&geometry,fl_project_scene(p,scene),1320,140,0)==ids[FL_WIDGET_ELLIPSE]);
    Fl_Id child=0;CHECK(fl_widget_create(p,scene,ids[0],FL_WIDGET_RECTANGLE,10,10,&child));
    CHECK(transform(p,scene,ids[0],(Fl_Pose){.x=100,.y=200,.rz=1.5707963267948966,.sx=2,.sy=2,.sz=1},(Fl_Bounds){320,180,0}));
    CHECK(fl_scene_geometry_update(&geometry,fl_project_scene(p,scene),NULL));
    CHECK(fl_scene_geometry_pick(&geometry,fl_project_scene(p,scene),60,240,0)==child);
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ENTITY_VISIBLE,.scene=scene,.entity=ids[0],.visible=false},NULL));
    CHECK(fl_scene_geometry_update(&geometry,fl_project_scene(p,scene),NULL));
    CHECK(fl_scene_geometry_pick(&geometry,fl_project_scene(p,scene),60,240,0)==0);
    CHECK(transform(p,scene,child,(Fl_Pose){.sy=1,.sz=1},(Fl_Bounds){100,100,0}));
    CHECK(fl_scene_geometry_update(&geometry,fl_project_scene(p,scene),NULL));
    const Fl_Scene *current=fl_project_scene(p,scene);const Fl_Entity *child_entity=fl_scene_entity(current,child);
    CHECK(!geometry.entities[child_entity-current->entities].invertible);
    Fl_Affine inverse;CHECK(!fl_affine_inverse((Fl_Affine){{0,0,0,0,1,0}},&inverse));
    const Fl_Entity *text=fl_scene_entity(fl_project_scene(p,scene),ids[FL_WIDGET_TEXT]);
    Fl_Typography typography=fl_widget_typography(text);
    CHECK(fl_entity_component(text,FL_COMPONENT_TYPOGRAPHY)&&!strcmp(typography.family,FL_TYPOGRAPHY_DEFAULT_FAMILY)&&typography.weight==400&&!typography.italic);
    Fl_Typography serif={"Georgia",700,true};unsigned char bytes[3+FL_TYPOGRAPHY_FAMILY_MAX];
    size_t size=fl_typography_encode(&serif,bytes);CHECK(size==3+strlen("Georgia"));
    Fl_Component typed={.type = FL_COMPONENT_TYPOGRAPHY, .version = 1, .size = size, .data = bytes};
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=ids[FL_WIDGET_TEXT],.component=typed},NULL));
    Fl_Typography bad=serif;bad.weight=0;CHECK(!fl_typography_encode(&bad,bytes));
    bad=serif;bad.family[0]=0;CHECK(!fl_typography_encode(&bad,bytes));
    unsigned char corrupt[]={0x90,0x01,2,'A'};Fl_Component corrupt_typography={.type = FL_COMPONENT_TYPOGRAPHY, .version = 1, .size = 4, .data = corrupt};
    CHECK(!fl_widget_component_valid(&corrupt_typography));
    unsigned char image[8];fl_image_asset_encode(4242,image);
    Fl_Component dangling={.type = FL_COMPONENT_IMAGE, .version = 1, .size = 8, .data = image};
    CHECK(fl_widget_component_valid(&dangling));
    CHECK(!fl_project_apply(p,&(Fl_Op){.kind=FL_OP_COMPONENT_SET,.scene=scene,.entity=ids[FL_WIDGET_RECTANGLE],.component=dangling},NULL));
    CHECK(!fl_widget_create_image(p,scene,0,4242,0,0,10,10,NULL));
    Fl_Asset font={.name="Font",.kind=FL_ASSET_FONT,.hash={1},.extension="ttf",.size=10};
    Fl_Asset picture={.name="Picture",.kind=FL_ASSET_IMAGE,.hash={2},.extension="png",.size=10,.width=64,.height=32};
    Fl_Id font_id,picture_id,image_entity;
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ASSET_ADD,.asset_record=&font},&font_id));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ASSET_ADD,.asset_record=&picture},&picture_id));
    CHECK(!fl_widget_create_image(p,scene,0,font_id,0,0,10,10,NULL));
    CHECK(!fl_widget_create_image(p,scene,0,picture_id,0,0,0,10,NULL));
    CHECK(fl_widget_create_image(p,scene,0,picture_id,5,6,64,32,&image_entity));
    const Fl_Entity *picture_entity=fl_scene_entity(fl_project_scene(p,scene),image_entity);
    Fl_Id referenced=0;CHECK(fl_widget_for_entity(picture_entity)->kind==FL_WIDGET_IMAGE&&!strcmp(picture_entity->name,"Picture"));
    CHECK(fl_image_asset_decode(fl_entity_component(picture_entity,FL_COMPONENT_IMAGE),&referenced)&&referenced==picture_id);
    CHECK(fl_project_asset_uses(p,picture_id)==1&&fl_project_asset_uses(p,font_id)==0);
    CHECK(!fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=picture_id},NULL));
    CHECK(fl_scene_geometry_update(&geometry,fl_project_scene(p,scene),NULL));
    CHECK(fl_scene_geometry_pick(&geometry,fl_project_scene(p,scene),30,20,0)==image_entity);
    /* Style and video components validate, reference assets and map scene time to source time. */
    Fl_Style look={.radius=24,.gradient=true,.gradient_to={1,2,3,4},.gradient_angle=1.5,.shadow_blur=30,.shadow_x=2,.shadow_y=8,.shadow_color={0,0,0,200},.align=FL_ALIGN_CENTER,.crop={0.25,0.5,0.5,0.25}},decoded_style;
    unsigned char style_bytes[FL_STYLE_BYTES];fl_style_encode(&look,style_bytes);
    Fl_Component style_component={.type = FL_COMPONENT_STYLE, .version = 1, .size = FL_STYLE_BYTES, .data = style_bytes};
    CHECK(fl_widget_component_valid(&style_component)&&fl_style_decode(&style_component,&decoded_style)&&decoded_style.radius==24&&decoded_style.align==FL_ALIGN_CENTER&&decoded_style.gradient_to[3]==4&&decoded_style.crop[2]==0.5);
    look.crop[2]=0.9;fl_style_encode(&look,style_bytes);CHECK(!fl_widget_component_valid(&style_component));look.crop[2]=0.5;fl_style_encode(&look,style_bytes);
    style_bytes[49]=9;CHECK(!fl_widget_component_valid(&style_component));
    look.radius=-1;fl_style_encode(&look,style_bytes);CHECK(!fl_widget_component_valid(&style_component));
    Fl_Asset movie={.name="Movie",.kind=FL_ASSET_VIDEO,.hash={3},.extension="mp4",.size=10,.width=1920,.height=1080,.duration=10*FL_TICKS_PER_SECOND};
    Fl_Id movie_id,video_entity;
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ASSET_ADD,.asset_record=&movie},&movie_id));
    CHECK(!fl_widget_create_video(p,scene,0,font_id,0,0,10,10,NULL)&&!fl_widget_create_video(p,scene,0,picture_id,0,0,10,10,NULL));
    CHECK(fl_widget_create_video(p,scene,0,movie_id,10,20,640,360,&video_entity));
    const Fl_Entity *video=fl_scene_entity(fl_project_scene(p,scene),video_entity);
    Fl_VideoClip clip;CHECK(fl_widget_for_entity(video)->kind==FL_WIDGET_VIDEO&&fl_video_clip_decode(fl_entity_component(video,FL_COMPONENT_VIDEO),&clip));
    CHECK(clip.asset==movie_id&&clip.rate==1&&!clip.loop&&fl_project_asset_uses(p,movie_id)==1);
    CHECK(!fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=movie_id},NULL));
    clip=(Fl_VideoClip){.asset=movie_id,.start=2*FL_TICKS_PER_SECOND,.offset=FL_TICKS_PER_SECOND,.rate=2};
    const uint64_t length=10*FL_TICKS_PER_SECOND;
    CHECK(fl_video_clip_source_seconds(&clip,0,length)==1&&fl_video_clip_source_seconds(&clip,3*FL_TICKS_PER_SECOND,length)==3&&fl_video_clip_source_seconds(&clip,60*FL_TICKS_PER_SECOND,length)==10);
    clip.loop=true;CHECK(fabs(fl_video_clip_source_seconds(&clip,8*FL_TICKS_PER_SECOND,length)-3)<1e-9);
    unsigned char clip_bytes[FL_VIDEO_CLIP_BYTES];clip.rate=0;fl_video_clip_encode(&clip,clip_bytes);
    Fl_Component clip_component={.type = FL_COMPONENT_VIDEO, .version = 1, .size = FL_VIDEO_CLIP_BYTES, .data = clip_bytes};CHECK(!fl_widget_component_valid(&clip_component));
    CHECK(fl_project_save(p));fl_project_destroy(p);p=fl_project_open(argv[1],NULL);CHECK(p);
    text=fl_scene_entity(fl_project_scene(p,scene),ids[FL_WIDGET_TEXT]);typography=fl_widget_typography(text);
    CHECK(!strcmp(typography.family,"Georgia")&&typography.weight==700&&typography.italic);
    CHECK(fl_project_asset_count(p)==3&&fl_project_asset_uses(p,picture_id)==1&&fl_project_asset_uses(p,movie_id)==1);
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ENTITY_REMOVE,.scene=scene,.entity=image_entity},NULL));
    CHECK(fl_project_apply(p,&(Fl_Op){.kind=FL_OP_ASSET_REMOVE,.asset=picture_id},NULL));
    ellipse=fl_scene_entity(fl_project_scene(p,scene),ids[FL_WIDGET_ELLIPSE]);CHECK(fl_widget_for_entity(ellipse)->kind==FL_WIDGET_ELLIPSE);
    CHECK(fl_widget_appearance(ellipse).stroke_width==12&&fl_entity_component(ellipse,999)->data[2]==3);
    fl_scene_geometry_clear(&geometry);fl_project_destroy(p);fl_fs_remove(path);
    return FLOW_TEST_RESULT("fl_widgets_tests");
}

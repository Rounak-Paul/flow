#include "fl_timeline_internal.h"
#include "schema/fl_widgets.h"
#include "schema/fl_properties.h"
#include "time/fl_scene_state.h"
#include <math.h>
#include <string.h>

/* Compare a proposed channel with its evaluated value at tick, ignoring numerical roundoff. */
bool fl_keyed_value_changed(const Fl_Scene *scene, const Fl_Track *track, Fl_Tick tick, const double value[4])
{
    double current[4];
    if (!fl_track_sample(scene, track, tick, current)) return true;
    int channels=(int)fl_property_definition(track->property)->channels;
    for (int c = 0; c < channels; ++c) {
        if (channels == 4) current[c] = lround(current[c]);
        if (fabs(current[c] - value[c]) > 1e-12 * fmax(1, fmax(fabs(current[c]), fabs(value[c])))) return true;
    }
    return false;
}

/* Split keyed configure components: animated channels become key writes and keep their base values.
   Rewritten components point at the caller's scratch buffers. Returns the number of writes. */
size_t fl_keyed_components(const Fl_Scene *s, const Fl_Entity *e, Fl_Component *components, size_t count,
                               unsigned char pose_bytes[FL_POSE_BYTES], unsigned char bounds_bytes[FL_BOUNDS_BYTES], unsigned char appearance[24], unsigned char opacity[8], unsigned char style[91],
                               Fl_KeyWrite *writes, Fl_Tick tick)
{
    if (!s || !e || count>64 || (count && !components)) return SIZE_MAX;
    for (size_t i=0;i<count;++i) {
        if (!fl_widget_component_valid(&components[i])) return SIZE_MAX;
        for (size_t j=0;j<i;++j) if ((components[i].type!=FL_COMPONENT_SCRIPT && components[i].type==components[j].type) || (components[i].id && components[i].id==components[j].id)) return SIZE_MAX;
    }
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        Fl_Component *c = &components[i];
        if (c->type == FL_COMPONENT_POSE) {
            Fl_Pose next, base;
            if (!fl_pose_decode(c, &next) || !fl_pose_decode(fl_entity_component(e, FL_COMPONENT_POSE), &base)) continue;
            double *value[12] = {&next.x,&next.y,&next.z,&next.rx,&next.ry,&next.rz,&next.sx,&next.sy,&next.sz,&next.px,&next.py,&next.pz};
            const double kept[12] = {base.x,base.y,base.z,base.rx,base.ry,base.rz,base.sx,base.sy,base.sz,base.px,base.py,base.pz};
            static const Fl_AnimProperty channels[12] = {FL_PROP_X,FL_PROP_Y,FL_PROP_Z,FL_PROP_RX,FL_PROP_RY,FL_PROP_RZ,FL_PROP_SCALE_X,FL_PROP_SCALE_Y,FL_PROP_SCALE_Z,FL_PROP_PIVOT_X,FL_PROP_PIVOT_Y,FL_PROP_PIVOT_Z};
            for (int k = 0; k < 12; ++k) if (fl_scene_track(s, e->id, channels[k])) {
                writes[n++] = (Fl_KeyWrite){e->id, channels[k], {*value[k], 0, 0, 0}};
                *value[k] = kept[k];
            }
            fl_pose_encode(&next, pose_bytes); c->data = pose_bytes;
        } else if (c->type == FL_COMPONENT_BOUNDS) {
            Fl_Bounds next, base = fl_entity_bounds(e);
            if (!fl_bounds_decode(c, &next)) continue;
            double *value[3] = {&next.width,&next.height,&next.depth};
            const double kept[3] = {base.width,base.height,base.depth};
            static const Fl_AnimProperty channels[3] = {FL_PROP_WIDTH,FL_PROP_HEIGHT,FL_PROP_DEPTH};
            for (int k = 0; k < 3; ++k) if (fl_scene_track(s, e->id, channels[k])) {
                writes[n++] = (Fl_KeyWrite){e->id, channels[k], {*value[k], 0, 0, 0}};
                *value[k] = kept[k];
            }
            fl_bounds_encode(&next, bounds_bytes); c->data = bounds_bytes;
        } else if (c->type == FL_COMPONENT_APPEARANCE) {
            Fl_Appearance next, base = fl_widget_appearance(e);
            if (!fl_appearance_decode(c, &next)) continue;
            if (fl_scene_track(s, e->id, FL_PROP_FILL)) {
                writes[n++] = (Fl_KeyWrite){e->id, FL_PROP_FILL, {next.fill[0], next.fill[1], next.fill[2], next.fill[3]}};
                memcpy(next.fill, base.fill, 4);
            }
            if (fl_scene_track(s, e->id, FL_PROP_STROKE)) {
                writes[n++] = (Fl_KeyWrite){e->id, FL_PROP_STROKE, {next.stroke[0], next.stroke[1], next.stroke[2], next.stroke[3]}};
                memcpy(next.stroke, base.stroke, 4);
            }
            if (fl_scene_track(s, e->id, FL_PROP_STROKE_WIDTH)) {
                writes[n++] = (Fl_KeyWrite){e->id, FL_PROP_STROKE_WIDTH, {next.stroke_width, 0, 0, 0}}; next.stroke_width = base.stroke_width;
            }
            if (fl_scene_track(s, e->id, FL_PROP_FONT_SIZE)) {
                writes[n++] = (Fl_KeyWrite){e->id, FL_PROP_FONT_SIZE, {next.font_size, 0, 0, 0}}; next.font_size = base.font_size;
            }
            fl_appearance_encode(&next, appearance); c->data = appearance;
        } else if (c->type==FL_COMPONENT_STYLE) {
            Fl_Style next,base=fl_entity_style(e); if (!fl_style_decode(c,&next)) continue;
            Fl_EntityState proposed={.style=next},authored={.style=base};
            memcpy(proposed.crop,next.crop,sizeof(next.crop)); memcpy(authored.crop,base.crop,sizeof(base.crop));
            double values[FL_PROP_COUNT][4]={{0}},kept[FL_PROP_COUNT][4]={{0}};
            fl_entity_state_values(&proposed,values); fl_entity_state_values(&authored,kept);
            for (int p=FL_PROP_CROP_X;p<FL_PROP_MEDIA_TIME;++p) if (fl_scene_track(s,e->id,(Fl_AnimProperty)p)) {
                writes[n]=(Fl_KeyWrite){.entity=e->id,.property=(Fl_AnimProperty)p}; memcpy(writes[n++].value,values[p],sizeof(values[p]));
                fl_entity_state_apply(&proposed,(Fl_AnimProperty)p,kept[p]);
            }
            memcpy(proposed.style.crop,proposed.crop,sizeof(proposed.crop)); fl_style_encode(&proposed.style,style); c->data=style;
        } else if (c->type == FL_COMPONENT_OPACITY) {
            double next;
            if (!fl_opacity_decode(c, &next)) continue;
            if (fl_scene_track(s, e->id, FL_PROP_OPACITY)) writes[n++] = (Fl_KeyWrite){e->id, FL_PROP_OPACITY, {next, 0, 0, 0}};
            else continue;
            fl_opacity_encode(fl_widget_opacity(e), opacity); c->data = opacity;
        }
    }
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i) if (fl_keyed_value_changed(s, fl_scene_track(s, e->id, writes[i].property), tick, writes[i].value))
        writes[kept++] = writes[i];
    return kept;
}


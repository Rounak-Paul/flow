#include "fl_spatial.h"
#include "project/fl_project.h"
#include "time/fl_scene_state.h"
#include "schema/fl_widgets.h"
#include <math.h>
#include <string.h>

Fl_Camera fl_camera_default(void) {return (Fl_Camera){.fov_y = 0.9, .near_z = 0.1, .far_z = 10000, .exposure = 1};}

void fl_camera_canvas_aspect(const Fl_Scene *scene, unsigned *aspect_w, unsigned *aspect_h)
{
    unsigned w = 16, h = 9;
    uint64_t width = scene ? scene->width : 0, height = scene ? scene->height : 0;
    if (width && height) {
        uint64_t a = width, b = height;
        while (b) { uint64_t t = a % b; a = b; b = t; }
        width /= a; height /= a;
        while ((width > 255 || height > 255) && !(width % 2) && !(height % 2)) { width /= 2; height /= 2; }
        if (width && height && width <= 255 && height <= 255) { w = (unsigned)width; h = (unsigned)height; }
    }
    if (aspect_w) *aspect_w = w;
    if (aspect_h) *aspect_h = h;
}
Fl_Material fl_material_default(void) {return (Fl_Material){.base = {220, 224, 232, 255}, .roughness = 0.55};}
Fl_Light fl_light_default(void) {return (Fl_Light){.color = {255, 248, 236, 255}, .intensity = 2, .spot_angle = 0.5};}
Fl_Particles fl_particles_default(void)
{
    Fl_Particles p = {128, 1, 24, 1.6, 14, 90, {255, 176, 64, 255}, 14, 40, 0.7, 0, 0, 0, 0, 0, {255, 176, 64, 255}};
    return p;
}
Fl_Post fl_post_default(void) {return (Fl_Post){0, 0, 0, 0, 1, 1};}

static void put_double(unsigned char *out, double value)
{
    uint64_t bits; memcpy(&bits, &value, 8);
    for (unsigned b = 0; b < 8; ++b) out[b] = (unsigned char)(bits >> (b * 8));
}
static bool get_double(const unsigned char *in, double *value)
{
    uint64_t bits = 0; for (unsigned b = 0; b < 8; ++b) bits |= (uint64_t)in[b] << (b * 8);
    memcpy(value, &bits, 8); return isfinite(*value);
}

void fl_camera_encode(const Fl_Camera *camera, unsigned char out[FL_CAMERA_BYTES])
{
    Fl_Camera c = camera ? *camera : fl_camera_default();
    memset(out, 0, FL_CAMERA_BYTES);
    out[0] = c.perspective ? 1 : 0;
    out[2] = (unsigned char)(c.aspect_w > 255 ? 255 : c.aspect_w);
    out[3] = (unsigned char)(c.aspect_h > 255 ? 255 : c.aspect_h);
    put_double(out + 8, c.fov_y); put_double(out + 16, c.near_z); put_double(out + 24, c.far_z);
    put_double(out + 32, c.ortho_height); put_double(out + 40, c.exposure);
}
bool fl_camera_decode(const Fl_Component *c, Fl_Camera *out)
{
    if (!c || !out || c->type != FL_COMPONENT_CAMERA || c->version != 1 || c->size != FL_CAMERA_BYTES || !c->data || c->data[0] > 1) return false;
    Fl_Camera camera = {.perspective = c->data[0] != 0, .aspect_w = c->data[2], .aspect_h = c->data[3]};
    if (!get_double(c->data + 8, &camera.fov_y) || !get_double(c->data + 16, &camera.near_z) || !get_double(c->data + 24, &camera.far_z) ||
        !get_double(c->data + 32, &camera.ortho_height) || !get_double(c->data + 40, &camera.exposure)) return false;
    if (camera.fov_y <= 0.05 || camera.fov_y >= 3 || camera.near_z <= 0 || camera.far_z <= camera.near_z || camera.ortho_height < 0 || camera.exposure <= 0 || camera.exposure > 64) return false;
    *out = camera; return true;
}

void fl_material_encode(const Fl_Material *material, unsigned char out[FL_MATERIAL_BYTES])
{
    Fl_Material m = material ? *material : fl_material_default();
    memset(out, 0, FL_MATERIAL_BYTES);
    memcpy(out, m.base, 4);
    put_double(out + 8, m.roughness); put_double(out + 16, m.metallic); put_double(out + 24, m.emissive);
}
bool fl_material_decode(const Fl_Component *c, Fl_Material *out)
{
    if (!c || !out || c->type != FL_COMPONENT_MATERIAL || c->version != 1 || c->size != FL_MATERIAL_BYTES || !c->data) return false;
    Fl_Material m; memcpy(m.base, c->data, 4);
    if (!get_double(c->data + 8, &m.roughness) || !get_double(c->data + 16, &m.metallic) || !get_double(c->data + 24, &m.emissive)) return false;
    if (m.roughness < 0 || m.roughness > 1 || m.metallic < 0 || m.metallic > 1 || m.emissive < 0 || m.emissive > 64) return false;
    *out = m; return true;
}

void fl_light_encode(const Fl_Light *light, unsigned char out[FL_LIGHT_BYTES])
{
    Fl_Light l = light ? *light : fl_light_default();
    memset(out, 0, FL_LIGHT_BYTES);
    out[0] = (unsigned char)l.kind; memcpy(out + 4, l.color, 4);
    put_double(out + 8, l.intensity); put_double(out + 16, l.range); put_double(out + 24, l.spot_angle);
}
bool fl_light_decode(const Fl_Component *c, Fl_Light *out)
{
    if (!c || !out || c->type != FL_COMPONENT_LIGHT || c->version != 1 || c->size != FL_LIGHT_BYTES || !c->data || c->data[0] > FL_LIGHT_SPOT) return false;
    Fl_Light l = {.kind = (Fl_LightKind)c->data[0]}; memcpy(l.color, c->data + 4, 4);
    if (!get_double(c->data + 8, &l.intensity) || !get_double(c->data + 16, &l.range) || !get_double(c->data + 24, &l.spot_angle)) return false;
    if (l.intensity < 0 || l.intensity > 100000 || l.range < 0 || l.spot_angle <= 0 || l.spot_angle >= 1.5) return false;
    *out = l; return true;
}

void fl_particles_encode(const Fl_Particles *particles, unsigned char out[FL_PARTICLES_BYTES])
{
    Fl_Particles p = particles ? *particles : fl_particles_default();
    memset(out, 0, FL_PARTICLES_BYTES);
    uint64_t capacity = p.capacity, seed = p.seed;
    for (unsigned b = 0; b < 8; ++b) {out[b] = (unsigned char)(capacity >> (8 * b)); out[8 + b] = (unsigned char)(seed >> (8 * b));}
    put_double(out + 16, p.rate); put_double(out + 24, p.life); put_double(out + 32, p.size); put_double(out + 40, p.speed);
    memcpy(out + 48, p.color, 4);
    put_double(out + 56, p.size_end); put_double(out + 64, p.gravity); put_double(out + 72, p.spread);
    put_double(out + 80, p.burst); put_double(out + 88, p.blend);
    put_double(out + 96, p.force_x); put_double(out + 104, p.force_y); put_double(out + 112, p.force_z);
    memcpy(out + 120, p.color_end, 4);
}
bool fl_particles_decode(const Fl_Component *c, Fl_Particles *out)
{
    if (!c || !out || c->type != FL_COMPONENT_PARTICLES || c->version != 1 || c->size != FL_PARTICLES_BYTES || !c->data) return false;
    Fl_Particles p = {0}; uint64_t capacity = 0, seed = 0;
    for (unsigned b = 0; b < 8; ++b) {capacity |= (uint64_t)c->data[b] << (8 * b); seed |= (uint64_t)c->data[8 + b] << (8 * b);}
    if (capacity > 8192) return false;
    p.capacity = (uint32_t)capacity; p.seed = (uint32_t)seed;
    if (!get_double(c->data + 16, &p.rate) || !get_double(c->data + 24, &p.life) || !get_double(c->data + 32, &p.size) || !get_double(c->data + 40, &p.speed)) return false;
    memcpy(p.color, c->data + 48, 4);
    if (!get_double(c->data + 56, &p.size_end) || !get_double(c->data + 64, &p.gravity) || !get_double(c->data + 72, &p.spread) ||
        !get_double(c->data + 80, &p.burst) || !get_double(c->data + 88, &p.blend) || !get_double(c->data + 96, &p.force_x) ||
        !get_double(c->data + 104, &p.force_y) || !get_double(c->data + 112, &p.force_z)) return false;
    memcpy(p.color_end, c->data + 120, 4);
    if (!p.capacity || p.capacity > 8192 || p.rate < 0 || p.rate > 10000 || p.life <= 0 || p.life > 120 || p.size <= 0 || p.size > 4096 || p.speed < 0 || p.speed > 100000) return false;
    if (p.size_end < 0 || p.size_end > 4096 || p.spread < 0 || p.spread > 4 || (p.burst != 0 && p.burst != 1) || (p.blend != 0 && p.blend != 1)) return false;
    *out = p; return true;
}

void fl_post_encode(const Fl_Post *post, unsigned char out[FL_POST_BYTES])
{
    Fl_Post p = post ? *post : fl_post_default();
    if (!post) p = fl_post_default();
    put_double(out, p.bloom); put_double(out + 8, p.vignette); put_double(out + 16, p.grain);
    put_double(out + 24, p.lift); put_double(out + 32, p.gamma); put_double(out + 40, p.gain);
}
bool fl_post_decode(const Fl_Component *c, Fl_Post *out)
{
    Fl_Post p = fl_post_default();
    if (!c || !out || c->type != FL_COMPONENT_POST || c->version != 1 || c->size != FL_POST_BYTES || !c->data) return false;
    if (!get_double(c->data, &p.bloom) || !get_double(c->data + 8, &p.vignette) || !get_double(c->data + 16, &p.grain) ||
        !get_double(c->data + 24, &p.lift) || !get_double(c->data + 32, &p.gamma) || !get_double(c->data + 40, &p.gain)) return false;
    if (p.bloom < 0 || p.bloom > 8 || p.vignette < 0 || p.vignette > 2 || p.grain < 0 || p.grain > 1 || p.lift < -1 || p.lift > 1 || p.gamma < 0.2 || p.gamma > 4 || p.gain < 0 || p.gain > 4) return false;
    *out = p; return true;
}
bool fl_mesh_decode(const Fl_Component *c, Fl_MeshShape *out)
{
    if (!c || !out || c->type != FL_COMPONENT_MESH || c->version != 1 || c->size != FL_MESH_BYTES || !c->data) return false;
    if (c->data[0] < FL_MESH_PLANE || c->data[0] > FL_MESH_SPHERE) return false;
    *out = (Fl_MeshShape)c->data[0]; return true;
}

Fl_Mat4 fl_camera_frame(double frame_x, double frame_y, double frame_width, double frame_height)
{
    return fl_mat4_ortho(frame_x, frame_x + frame_width, frame_y, frame_y + frame_height, -10000, 10000);
}

Fl_Mat4 fl_camera_view_proj(const Fl_Pose *pose, const Fl_Camera *camera, double frame_x, double frame_y, double frame_width, double frame_height)
{
    Fl_Camera c = camera ? *camera : fl_camera_default();
    if (!c.perspective) {
        if (c.ortho_height <= 0) return fl_camera_frame(frame_x, frame_y, frame_width, frame_height);
        double height = c.ortho_height;
        double width = frame_height > 0 ? height * frame_width / frame_height : height;
        Fl_Pose p = pose ? *pose : fl_pose_identity();
        return fl_mat4_ortho(p.x - width * 0.5, p.x + width * 0.5, p.y - height * 0.5, p.y + height * 0.5, c.near_z, c.far_z);
    }
    Fl_Mat4 view, world = fl_pose_model(pose ? pose : &(Fl_Pose){.sx = 1, .sy = 1, .sz = 1});
    if (!fl_mat4_inverse(world, &view)) view = fl_mat4_identity();
    double aspect = frame_height > 0 ? frame_width / frame_height : 1;
    return fl_mat4_mul(fl_mat4_perspective(c.fov_y, aspect, c.near_z, c.far_z), view);
}

static uint32_t mix_hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x;
}

size_t fl_particles_sample(const Fl_Particles *system, double seconds, Fl_Particle *out, size_t capacity)
{
    if (!system || !out || !capacity || system->rate <= 0 || system->life <= 0) return 0;
    double window = system->burst > 0.5 ? system->life : system->life;
    size_t alive = system->burst > 0.5 ? system->capacity : (size_t)(system->rate * window);
    if (alive > system->capacity) alive = system->capacity;
    if (alive > capacity) alive = capacity;
    if (alive > 2048) alive = 2048;
    size_t count = 0;
    double spread_scale = system->spread > 0 ? system->spread : 0.7;
    for (size_t i = 0; i < alive; ++i) {
        double stagger = window * (double)i / (double)(alive ? alive : 1);
        double age = system->burst > 0.5 ? seconds : (seconds >= 0 ? fmod(seconds + stagger, window) : 0);
        if (system->burst > 0.5 && (age < 0 || age > system->life)) continue;
        if (age < 0) age += window;
        uint32_t h = mix_hash(system->seed ^ (uint32_t)(i * 0x9e3779b9u));
        double u = (h & 65535) / 65535.0, v = ((h >> 16) & 65535) / 65535.0;
        double theta = u * 6.283185307179586, spread = (v - 0.5) * spread_scale;
        double len = sqrt(fmax(0, 1 - spread * spread));
        double dx = cos(theta) * spread, dz = sin(theta) * spread, dy = -len;
        double travel = system->speed * age;
        double t = system->life > 0 ? fmin(1, age / system->life) : 1;
        double size = system->size + (system->size_end - system->size) * t;
        unsigned char color[4];
        for (int c = 0; c < 4; ++c) color[c] = (unsigned char)lround(system->color[c] + ((int)system->color_end[c] - (int)system->color[c]) * t);
        out[count++] = (Fl_Particle){dx * travel + system->force_x * age, dy * travel + system->gravity * age * age + system->force_y * age,
                                     dz * travel + system->force_z * age, age, system->life, size, {color[0], color[1], color[2], color[3]}};
    }
    return count;
}

static bool scene_camera(const Fl_Scene *scene, const Fl_SceneState *state, size_t index, Fl_Pose *pose, Fl_Camera *camera, double *bloom)
{
    const Fl_Entity *e = &scene->entities[index];
    if (!e->visible) return false;
    if (state && state->valid && index < state->count && state->entities[index].has_camera) {
        if (state->entities[index].opacity <= 0) return false;
        *pose = state->entities[index].pose; *camera = state->entities[index].camera;
        *bloom = state->entities[index].has_post ? state->entities[index].post.bloom : 0;
        return true;
    }
    if (!fl_camera_decode(fl_entity_component(e, FL_COMPONENT_CAMERA), camera)) return false;
    fl_pose_decode(fl_entity_component(e, FL_COMPONENT_POSE), pose);
    Fl_Post post;
    *bloom = fl_post_decode(fl_entity_component(e, FL_COMPONENT_POST), &post) ? post.bloom : 0;
    return true;
}

bool fl_scene_camera_entity(const Fl_Scene *scene, const Fl_SceneState *state, Fl_Id id, Fl_Pose *pose, Fl_Camera *camera, double *bloom)
{
    const Fl_Entity *entity = scene && id ? fl_scene_entity(scene, id) : NULL;
    if (!entity || !pose || !camera) return false;
    size_t index = (size_t)(entity - scene->entities);
    double strength = 0;
    if (state && state->valid && index < state->count && state->entities[index].has_camera) {
        *pose = state->entities[index].pose;
        *camera = state->entities[index].camera;
        strength = state->entities[index].has_post ? state->entities[index].post.bloom : 0;
    } else if (!fl_camera_decode(fl_entity_component(entity, FL_COMPONENT_CAMERA), camera)) return false;
    else {
        fl_pose_decode(fl_entity_component(entity, FL_COMPONENT_POSE), pose);
        Fl_Post post;
        strength = fl_post_decode(fl_entity_component(entity, FL_COMPONENT_POST), &post) ? post.bloom : 0;
    }
    pose->sx = pose->sy = pose->sz = 1; pose->px = pose->py = pose->pz = 0;
    if (bloom) *bloom = strength;
    return true;
}

bool fl_scene_view_camera(const Fl_Scene *scene, const Fl_SceneState *state, Fl_Pose *pose, Fl_Camera *camera, double *bloom)
{
    if (!scene || !pose || !camera) return false;
    for (size_t i = 0; i < scene->entity_count; ++i) {
        double strength = 0;
        if (!scene_camera(scene, state, i, pose, camera, &strength)) continue;
        pose->sx = pose->sy = pose->sz = 1; pose->px = pose->py = pose->pz = 0;
        if (bloom) *bloom = strength;
        return true;
    }
    return false;
}

void fl_scene_camera(const Fl_Scene *scene, const Fl_SceneState *state, double frame_x, double frame_y, double frame_width, double frame_height,
                     Fl_Mat4 *view_proj, Fl_Vec3 *right, Fl_Vec3 *up, double *bloom)
{
    Fl_Mat4 vp = fl_camera_frame(frame_x, frame_y, frame_width, frame_height);
    Fl_Vec3 cam_right = {1, 0, 0}, cam_up = {0, 1, 0};
    double strength = 0;
    Fl_Pose pose = fl_pose_identity(); Fl_Camera camera;
    if (fl_scene_view_camera(scene, state, &pose, &camera, &strength)) {
        Fl_Mat4 axes = fl_pose_model(&pose);
        cam_right = (Fl_Vec3){axes.m[0], axes.m[1], axes.m[2]};
        cam_up = (Fl_Vec3){axes.m[4], axes.m[5], axes.m[6]};
        vp = fl_camera_view_proj(&pose, &camera, frame_x, frame_y, frame_width, frame_height);
    }
    if (view_proj) *view_proj = vp;
    if (right) *right = cam_right;
    if (up) *up = cam_up;
    if (bloom) *bloom = strength;
}

Fl_SceneFrame fl_scene_gate(const Fl_Scene *scene, const Fl_SceneState *state)
{
    Fl_SceneFrame frame = fl_scene_frame(scene);
    if (!scene) return frame;
    Fl_Pose pose = fl_pose_identity(); Fl_Camera camera;
    if (!fl_scene_view_camera(scene, state, &pose, &camera, NULL)) return frame;
    uint64_t aspect_w = camera.aspect_w && camera.aspect_h ? camera.aspect_w : 0;
    uint64_t aspect_h = camera.aspect_w && camera.aspect_h ? camera.aspect_h : 0;
    double width = (double)scene->width, height = (double)scene->height;
    if (aspect_w && aspect_h) {
        double ratio = (double)aspect_w / (double)aspect_h;
        if (width / height > ratio) width = height * ratio;
        else height = width / ratio;
    }
    return (Fl_SceneFrame){(scene->width - width) * .5, (scene->height - height) * .5, width, height};
}

bool fl_spatial_create(Fl_Project *project, Fl_Id scene, Fl_Id parent, const char *kind, double x, double y, double z, Fl_Id *out_id)
{
    if (!kind || !isfinite(x) || !isfinite(y) || !isfinite(z)) return false;
    Fl_Pose pose = fl_pose_identity(); pose.x = x; pose.y = y; pose.z = z;
    unsigned char pose_bytes[FL_POSE_BYTES], bounds_bytes[FL_BOUNDS_BYTES], extra[FL_PARTICLES_BYTES], post[FL_POST_BYTES];
    Fl_Bounds bounds = {180, 180, 180};
    const char *name = kind;
    Fl_Component components[4];
    size_t count = 0;
    uint64_t extra_type = 0; size_t extra_size = 0;
    if (!strcmp(kind, "camera")) {
        const Fl_Scene *s = fl_project_scene(project, scene);
        pose.x = s ? (double)s->width * 0.5 : 0; pose.y = s ? (double)s->height * 0.5 : 0;
        pose.z = s ? (double)s->height : 1080; pose.x += x; pose.y += y; pose.z += z;
        Fl_Camera camera = fl_camera_default(); camera.perspective = true;
        fl_camera_canvas_aspect(s, &camera.aspect_w, &camera.aspect_h);
        fl_camera_encode(&camera, extra); extra_type = FL_COMPONENT_CAMERA; extra_size = FL_CAMERA_BYTES;
        bounds = (Fl_Bounds){0}; name = "Camera";
    } else if (!strcmp(kind, "plane") || !strcmp(kind, "cube") || !strcmp(kind, "sphere")) {
        extra[0] = (unsigned char)(!strcmp(kind, "plane") ? FL_MESH_PLANE : !strcmp(kind, "cube") ? FL_MESH_CUBE : FL_MESH_SPHERE);
        extra_type = FL_COMPONENT_MESH; extra_size = FL_MESH_BYTES;
        if (!strcmp(kind, "plane")) bounds = (Fl_Bounds){320, 180, 1};
        name = !strcmp(kind, "plane") ? "Plane" : !strcmp(kind, "cube") ? "Cube" : "Sphere";
        unsigned char material[FL_MATERIAL_BYTES]; fl_material_encode(NULL, material);
        fl_pose_encode(&pose, pose_bytes); fl_bounds_encode(&bounds, bounds_bytes);
        components[0] = (Fl_Component){.type = FL_COMPONENT_POSE, .version = 1, .size = FL_POSE_BYTES, .data = pose_bytes};
        components[1] = (Fl_Component){.type = FL_COMPONENT_BOUNDS, .version = 1, .size = FL_BOUNDS_BYTES, .data = bounds_bytes};
        components[2] = (Fl_Component){.type = FL_COMPONENT_MESH, .version = 1, .size = 1, .data = extra};
        components[3] = (Fl_Component){.type = FL_COMPONENT_MATERIAL, .version = 1, .size = FL_MATERIAL_BYTES, .data = material};
        return fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = scene, .parent = parent, .name = name, .components = components, .component_count = 4}, out_id);
    } else if (!strcmp(kind, "light")) {
        fl_light_encode(NULL, extra); extra_type = FL_COMPONENT_LIGHT; extra_size = FL_LIGHT_BYTES;
        pose.rx = -0.8; bounds = (Fl_Bounds){0}; name = "Light";
    } else if (!strcmp(kind, "particles")) {
        fl_particles_encode(NULL, extra); extra_type = FL_COMPONENT_PARTICLES; extra_size = FL_PARTICLES_BYTES;
        bounds = (Fl_Bounds){0}; name = "Particles";
    } else return false;
    fl_pose_encode(&pose, pose_bytes); fl_bounds_encode(&bounds, bounds_bytes);
    components[count++] = (Fl_Component){.type = FL_COMPONENT_POSE, .version = 1, .size = FL_POSE_BYTES, .data = pose_bytes};
    if (bounds.width || bounds.height || bounds.depth) components[count++] = (Fl_Component){.type = FL_COMPONENT_BOUNDS, .version = 1, .size = FL_BOUNDS_BYTES, .data = bounds_bytes};
    components[count++] = (Fl_Component){.type = extra_type, .version = 1, .size = extra_size, .data = extra};
    if (extra_type == FL_COMPONENT_CAMERA) {
        Fl_Post grade = fl_post_default(); grade.bloom = 0.35;
        fl_post_encode(&grade, post);
        components[count++] = (Fl_Component){.type = FL_COMPONENT_POST, .version = 1, .size = FL_POST_BYTES, .data = post};
    }
    return fl_project_apply(project, &(Fl_Op){.kind = FL_OP_ENTITY_ADD, .scene = scene, .parent = parent, .name = name, .components = components, .component_count = count}, out_id);
}

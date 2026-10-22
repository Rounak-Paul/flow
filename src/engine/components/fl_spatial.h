#pragma once
#include "fl_transform.h"
#include "doc/fl_scene_frame.h"

#define FL_CAMERA_BYTES 48u
#define FL_MATERIAL_BYTES 32u
#define FL_LIGHT_BYTES 32u
#define FL_PARTICLES_BYTES 128u
#define FL_POST_BYTES 48u
#define FL_MESH_BYTES 1u

typedef enum Fl_MeshShape { FL_MESH_PLANE = 1, FL_MESH_CUBE = 2, FL_MESH_SPHERE = 3 } Fl_MeshShape;
typedef enum Fl_LightKind { FL_LIGHT_DIRECTIONAL = 0, FL_LIGHT_POINT = 1, FL_LIGHT_SPOT = 2 } Fl_LightKind;

typedef struct Fl_Camera {
    bool perspective;
    double fov_y, near_z, far_z, ortho_height, exposure;
    unsigned aspect_w, aspect_h; /* film gate; both zero use the canvas */
} Fl_Camera;
typedef struct Fl_Material {
    unsigned char base[4];
    double roughness, metallic, emissive;
} Fl_Material;
typedef struct Fl_Light {
    Fl_LightKind kind;
    unsigned char color[4];
    double intensity, range, spot_angle;
} Fl_Light;
typedef struct Fl_Particles {
    uint32_t capacity, seed;
    double rate, life, size, speed;
    unsigned char color[4];
    double size_end, gravity, spread, burst, blend, force_x, force_y, force_z;
    unsigned char color_end[4];
} Fl_Particles;
typedef struct Fl_Post {
    double bloom, vignette, grain, lift, gamma, gain;
} Fl_Post;
typedef struct Fl_Particle {
    double x, y, z, age, life, size;
    unsigned char color[4];
} Fl_Particle;

Fl_Camera fl_camera_default(void);
/* Canvas ratio reduced to fit a camera's aspect bytes. 16:9 when the canvas has no size. */
void fl_camera_canvas_aspect(const Fl_Scene *scene, unsigned *aspect_w, unsigned *aspect_h);
Fl_Material fl_material_default(void);
Fl_Light fl_light_default(void);
Fl_Particles fl_particles_default(void);
Fl_Post fl_post_default(void);

void fl_camera_encode(const Fl_Camera *camera, unsigned char out[FL_CAMERA_BYTES]);
bool fl_camera_decode(const Fl_Component *component, Fl_Camera *out);
void fl_material_encode(const Fl_Material *material, unsigned char out[FL_MATERIAL_BYTES]);
bool fl_material_decode(const Fl_Component *component, Fl_Material *out);
void fl_light_encode(const Fl_Light *light, unsigned char out[FL_LIGHT_BYTES]);
bool fl_light_decode(const Fl_Component *component, Fl_Light *out);
void fl_particles_encode(const Fl_Particles *particles, unsigned char out[FL_PARTICLES_BYTES]);
bool fl_particles_decode(const Fl_Component *component, Fl_Particles *out);
void fl_post_encode(const Fl_Post *post, unsigned char out[FL_POST_BYTES]);
bool fl_post_decode(const Fl_Component *component, Fl_Post *out);
bool fl_mesh_decode(const Fl_Component *component, Fl_MeshShape *out);

typedef struct Fl_Scene Fl_Scene;
typedef struct Fl_SceneState Fl_SceneState;

/* View-projection for a camera entity. A positive ortho_height looks along the camera's local -Z;
   near and far are distances along that axis. ortho_height 0 keeps the scene frame. */
Fl_Mat4 fl_camera_view_proj(const Fl_Pose *pose, const Fl_Camera *camera, double frame_x, double frame_y, double frame_width, double frame_height);
/* Orthographic projection of one scene rectangle. An orthographic camera with ortho_height 0 uses this. */
Fl_Mat4 fl_camera_frame(double frame_x, double frame_y, double frame_width, double frame_height);
/* The first visible camera. False when the scene has none; outputs are written only on success.
   right/up are billboard axes. bloom is the camera post strength. */
bool fl_scene_camera(const Fl_Scene *scene, const Fl_SceneState *state, double frame_x, double frame_y,
                     double frame_width, double frame_height, Fl_Mat4 *view_proj, Fl_Vec3 *right, Fl_Vec3 *up, double *bloom);
/* The first visible camera entity. False when the scene has none. */
bool fl_scene_view_camera(const Fl_Scene *scene, const Fl_SceneState *state, Fl_Pose *pose, Fl_Camera *camera, double *bloom);
/* One camera entity, including a hidden one chosen from the viewport. False when id is not a camera. */
bool fl_scene_camera_entity(const Fl_Scene *scene, const Fl_SceneState *state, Fl_Id id, Fl_Pose *pose, Fl_Camera *camera, double *bloom);
/* Output gate. The first visible camera's aspect crops the canvas; both zero keep the whole canvas. */
Fl_SceneFrame fl_scene_gate(const Fl_Scene *scene, const Fl_SceneState *state);

/* Deterministic emission. out receives at most capacity alive particles in emitter-local space. */
size_t fl_particles_sample(const Fl_Particles *system, double seconds, Fl_Particle *out, size_t capacity);

typedef struct Fl_Project Fl_Project;
/* Create a camera, mesh, light or particle entity. kind is "camera", "plane", "cube", "sphere", "light" or "particles". */
bool fl_spatial_create(Fl_Project *project, Fl_Id scene, Fl_Id parent, const char *kind, double x, double y, double z, Fl_Id *out_id);

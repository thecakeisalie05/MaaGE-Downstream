#ifndef MAAGE_CLIENT_SHADER_SCENE_H
#define MAAGE_CLIENT_SHADER_SCENE_H
#include "maage/render.h"
#include "maage/world.h"
#include "maage/session_protocol.h"
struct maage_client_item_entities;
typedef struct maage_client_shader_scene {
    maage_render_light *lights;
    size_t capacity, count;
    double began;
    int supported;
    float immersion;
    unsigned long session, epoch;
} maage_client_shader_scene;
void maage_client_shader_light_offer(maage_render_light *lights, size_t *count, size_t capacity, const double eye[3], const double position[3], const float color[3], float intensity, float radius);
void maage_client_shader_light_state(maage_render_light *lights, size_t *count, size_t capacity, const double eye[3], const double position[3], const maage_block_registry *blocks, const maage_block_state_definition *state);
maage_status maage_client_shader_scene_update(maage_client_shader_scene *scene, maage_renderer *renderer, const maage_shader_settings *settings, int enabled, const maage_session_snapshot *snapshot, const maage_world *world, const maage_item_registry *items, struct maage_client_item_entities *entities, const double camera[3], double now, double elapsed, unsigned int hotbar);
void maage_client_shader_scene_destroy(maage_client_shader_scene *scene);
#endif

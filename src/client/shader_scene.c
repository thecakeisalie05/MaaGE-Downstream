#include "shader_scene.h"
#include "item_entities.h"
#include "resources.h"
#include "maage/environment.h"
#include "maage/fluids.h"
#include "maage/dimensions.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
static double shader_light_score(const maage_render_light *light, const double eye[3]) {
    double x = light->position_radius[0] - eye[0], y = light->position_radius[1] - eye[1], z = light->position_radius[2] - eye[2];
    return light->color_intensity[3] / (1 + x * x + y * y + z * z);
}
void maage_client_shader_light_offer(maage_render_light *lights, size_t *count, size_t capacity, const double eye[3], const double position[3], const float color[3], float intensity, float radius) {
    maage_render_light candidate;
    size_t i, lowest = 0;
    double score, minimum;
    unsigned int axis;
    if (!capacity || intensity <= 0 || radius <= 0) return;
    for (axis = 0; axis < 3; ++axis) {
        candidate.position_radius[axis] = (float) position[axis];
        candidate.color_intensity[axis] = color[axis];
    }
    candidate.position_radius[3] = radius;
    candidate.color_intensity[3] = intensity;
    score = shader_light_score(&candidate, eye);
    /* Merge adjacent luminous cells on a stable 2-block lattice; large lava
     * surfaces must not exhaust the entire budget before a held light arrives. */
    for (i = 0; i < *count; ++i) {
        if ((int) floor(lights[i].position_radius[0] * 0.5f) == (int) floor(position[0] * 0.5) && (int) floor(lights[i].position_radius[1] * 0.5f) == (int) floor(position[1] * 0.5) && (int) floor(lights[i].position_radius[2] * 0.5f) == (int) floor(position[2] * 0.5)) {
            if (score > shader_light_score(&lights[i], eye)) lights[i] = candidate;
            return;
        }
    }
    if (*count < capacity) {
        lights[(*count)++] = candidate;
        return;
    }
    minimum = shader_light_score(&lights[0], eye);
    for (i = 1; i < *count; ++i) {
        double value = shader_light_score(&lights[i], eye);
        if (value < minimum) {
            minimum = value;
            lowest = i;
        }
    }
    if (score > minimum) lights[lowest] = candidate;
}
void maage_client_shader_light_state(maage_render_light *lights, size_t *count, size_t capacity, const double eye[3], const double position[3], const maage_block_registry *blocks, const maage_block_state_definition *state) {
    static const float warm[3] = {1.0f, 0.57f, 0.25f};
    const maage_block_definition *block;
    const float *color;
    if (!state || !state->emitted_light) return;
    block = maage_block_registry_block(blocks, state->block_id);
    color = block && (block->emission_color[0] > 0 || block->emission_color[1] > 0 || block->emission_color[2] > 0) ? block->emission_color : warm;
    maage_client_shader_light_offer(lights, count, capacity, eye, position, color, state->emitted_light / 15.0f * 3.0f, (float) state->emitted_light);
}
maage_status maage_client_shader_scene_update(maage_client_shader_scene *scene, maage_renderer *renderer, const maage_shader_settings *settings, int enabled, const maage_session_snapshot *snapshot, const maage_world *world, const maage_item_registry *items, maage_client_item_entities *entities, const double camera[3], double now, double elapsed, unsigned int hotbar) {
    maage_shader_frame frame;
    maage_render_capabilities capabilities;
    const maage_block_registry *blocks = maage_world_registry(world);
    const maage_fluid_definition *fluid;
    const maage_item_definition *item;
    const maage_item_stack *held;
    maage_render_light *grown;
    size_t capacity, i;
    const char *override;
    char *end;
    unsigned long requested;
    double angle, visibility, sun, moon;
    int x, y, z, cx, cy, cz;
    memset(&frame, 0, sizeof(frame));
    maage_renderer_get_capabilities(renderer, &capabilities);
    scene->supported = (capabilities.features & MAAGE_RENDER_CAP_WORLD_EFFECTS) != 0;
    if (!scene->epoch || scene->session != snapshot->session_epoch || scene->epoch != snapshot->world_epoch) {
        scene->session = snapshot->session_epoch;
        scene->epoch = snapshot->world_epoch;
        scene->began = now;
        scene->immersion = 0;
    }
    for (i = 0; i < 3; ++i)
        frame.camera[i] = (float) camera[i];
    frame.time_seconds = (float) (now - scene->began);
    angle = maage_environment_celestial_angle(&snapshot->environment) * 6.283185307179586;
    frame.sun[0] = (float) -sin(angle);
    frame.sun[1] = (float) cos(angle);
    frame.moon[0] = -frame.sun[0];
    frame.moon[1] = -frame.sun[1];
    visibility = (1 - snapshot->environment.rain_intensity * 0.7) * (1 - snapshot->environment.thunder_intensity * 0.5);
    sun = frame.sun[1] > 0 ? frame.sun[1] : 0;
    moon = frame.moon[1] > 0 ? frame.moon[1] : 0;
    frame.weather[0] = (float) snapshot->environment.rain_intensity;
    frame.weather[1] = (float) snapshot->environment.thunder_intensity;
    frame.weather[2] = maage_world_dimension(world)->celestial != 0;
    frame.sun[3] = (float) (sun * visibility);
    frame.moon[3] = (float) (moon * visibility * 0.08 * (0.35 + 0.65 * (0.5 + 0.5 * cos((snapshot->environment.day_low & 7UL) * 0.7853981633974483))));
    if (!frame.weather[2]) frame.sun[3] = frame.moon[3] = 0;
    frame.sun_color[0] = 1;
    frame.sun_color[1] = (float) (0.5 + 0.47 * sqrt(sun));
    frame.sun_color[2] = (float) (0.28 + 0.64 * sqrt(sun));
    frame.moon_color[0] = 0.48f;
    frame.moon_color[1] = 0.62f;
    frame.moon_color[2] = 0.9f;
    fluid = maage_fluid_get(maage_block_registry_fluids(blocks), maage_world_fluid_at(world, camera));
    if (elapsed < 0) elapsed = 0;
    if (elapsed > 0.1) elapsed = 0.1;
    scene->immersion += ((fluid && fluid->shader_water ? 1.0f : 0.0f) - scene->immersion) * (float) (1 - exp(-elapsed * 9));
    frame.weather[3] = enabled && settings->underwater ? scene->immersion : 0;
    scene->count = 0;
    if (enabled && settings->dynamic_lights && (capabilities.features & MAAGE_RENDER_CAP_WORLD_EFFECTS)) {
        capacity = (size_t) (8U << settings->light_quality);
        override = getenv("MAAGE_SHADER_LIGHT_BUDGET");
        if (override && *override) {
            errno = 0;
            requested = strtoul(override, &end, 10);
            if (!errno && !*end && requested && requested <= (size_t) - 1 / sizeof(*grown)) capacity = (size_t) requested;
        }
        if (capacity > scene->capacity) {
            grown = (maage_render_light *) realloc(scene->lights, capacity * sizeof(*grown));
            if (!grown) return maage_status_make(MAAGE_STATUS_ALLOCATION_FAILED, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_UPDATE, 0);
            scene->lights = grown;
            scene->capacity = capacity;
        }
        /* Fixed, bounded owner queries of confirmed nearby data; no World lock,
         * stream request, filesystem call or propagated-light recalculation. */
        cx = (int) floor(camera[0]);
        cy = (int) floor(camera[1]);
        cz = (int) floor(camera[2]);
        for (z = -6; z <= 6; ++z)
            for (y = -5; y <= 5; ++y)
                for (x = -6; x <= 6; ++x) {
                    const maage_block_state_definition *state = maage_block_registry_state(blocks, maage_world_get(world, cx + x, cy + y, cz + z));
                    double position[3];
                    if (!state || !state->emitted_light) continue;
                    position[0] = cx + x + 0.5;
                    position[1] = cy + y + 0.65;
                    position[2] = cz + z + 0.5;
                    maage_client_shader_light_state(scene->lights, &scene->count, capacity, camera, position, blocks, state);
                }
        maage_client_item_entities_lights(entities, scene->lights, &scene->count, capacity, camera, now);
        held = &snapshot->player.inventory.slots[hotbar < MAAGE_HOTBAR_SLOTS ? hotbar : 0];
        item = held->count ? maage_item_registry_get(items, held->item_id) : NULL;
        if (item) {
            double position[3];
            for (i = 0; i < 3; ++i)
                position[i] = camera[i];
            position[1] -= 0.3;
            maage_client_shader_light_state(scene->lights, &scene->count, capacity, camera, position, blocks, maage_client_item_icon_state(blocks, item, held->variant));
        }
        frame.lights = scene->lights;
        frame.light_count = scene->count;
    }
    return maage_renderer_set_shader_effects(renderer, settings, &frame);
}
void maage_client_shader_scene_destroy(maage_client_shader_scene *scene) {
    if (scene) {
        free(scene->lights);
        memset(scene, 0, sizeof(*scene));
    }
}

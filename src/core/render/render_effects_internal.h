#ifndef MAAGE_RENDER_EFFECTS_INTERNAL_H
#define MAAGE_RENDER_EFFECTS_INTERNAL_H
#include "render_shared.h"
/* std140 matrices/vec4 only. Lights use their independent std430 frame buffer;
 * neither structure changes the portable vertex or 128-byte push-constant ABI. */
typedef struct maage_effect_uniforms {
    float view_projection[16], inverse_view_projection[16], shadows[4][16];
    float camera_time[4], sun[4], moon[4], sun_color[4], moon_color[4];
    float weather[4], wind[4], features[4], quality[4], splits[4], viewport[4], ambient[4];
} maage_effect_uniforms;
typedef char maage_effect_uniform_layout_check[sizeof(maage_effect_uniforms) == 576 ? 1 : -1];
maage_status maage_render_effect_uniforms_prepare(const maage_render_frame_plan *plan, unsigned int width, unsigned int height, unsigned int cascades, unsigned int resolution, maage_effect_uniforms *uniforms);
int maage_render_effect_bounds_visible(const float bounds[6], const float matrix[16]);
#endif

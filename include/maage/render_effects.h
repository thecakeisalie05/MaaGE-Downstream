#ifndef MAAGE_RENDER_EFFECTS_H
#define MAAGE_RENDER_EFFECTS_H
#include "maage/status.h"
#include <stddef.h>
/* Numeric presentation settings. These never alter World light or simulation. */
#define MAAGE_SHADER_CONTROLS(X) \
    X(shadows, 0, 1) \
    X(shadow_quality, 0, 3) \
    X(shadow_distance, 16, 256) \
    X(dynamic_lights, 0, 1) \
    X(light_quality, 0, 2) \
    X(waving, 0, 1) \
    X(wind, 0, 100) \
    X(god_rays, 0, 1) \
    X(ray_quality, 0, 2) \
    X(sky, 0, 1) \
    X(water, 0, 1) \
    X(water_quality, 0, 2) \
    X(underwater, 0, 1)
#define MAAGE_SHADER_FIELD(name, minimum, maximum) int name;
typedef struct maage_shader_settings {
    MAAGE_SHADER_CONTROLS(MAAGE_SHADER_FIELD)
} maage_shader_settings;
#undef MAAGE_SHADER_FIELD
#define MAAGE_SHADER_INDEX(name, minimum, maximum) MAAGE_SHADER_##name,
typedef enum maage_shader_control {
    MAAGE_SHADER_CONTROLS(MAAGE_SHADER_INDEX)
    MAAGE_SHADER_CONTROL_COUNT
} maage_shader_control;
#undef MAAGE_SHADER_INDEX
maage_status maage_shader_settings_preset(maage_shader_settings *settings, int preset);
int maage_shader_settings_valid(const maage_shader_settings *settings);
int maage_shader_settings_classify(const maage_shader_settings *settings); /* 0..2 presets, 3 Custom. */
int maage_shader_setting_get(const maage_shader_settings *settings, unsigned int control);
maage_status maage_shader_setting_set(maage_shader_settings *settings, unsigned int control, int value);
/* Frame-owned light descriptions; positions/radii and RGB/intensity are linear.
 * The Client prioritizes sources before submission. No permanent light-count
 * ceiling is encoded in this API; native buffer limits validate admission. */
typedef struct maage_render_light {
    float position_radius[4], color_intensity[4];
} maage_render_light;
typedef struct maage_shader_frame {
    float camera[3], time_seconds;
    float sun[4], moon[4], sun_color[4], moon_color[4];
    float weather[4]; /* rain, thunder, celestial enabled, immersion 0..1. */
    const maage_render_light *lights;
    size_t light_count;
} maage_shader_frame;
#endif

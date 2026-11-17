#include "maage/render_effects.h"
maage_status maage_shader_settings_preset(maage_shader_settings *settings, int preset) {
    if (!settings || preset < 0 || preset > 2) return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_RENDER, MAAGE_OPERATION_SET, 0);
    settings->shadows = settings->dynamic_lights = settings->waving = settings->god_rays = settings->sky = settings->water = settings->underwater = 1;
    settings->shadow_quality = preset == 2 ? 3 : preset;
    settings->shadow_distance = preset == 0 ? 48 : (preset == 1 ? 96 : 160);
    settings->light_quality = settings->ray_quality = settings->water_quality = preset;
    settings->wind = preset == 0 ? 25 : (preset == 1 ? 40 : 55);
    return maage_status_make(MAAGE_STATUS_SUCCESS, 0, MAAGE_OPERATION_SET, 0);
}
int maage_shader_settings_valid(const maage_shader_settings *settings) {
    if (!settings) return 0;
#define MAAGE_VALIDATE(name, minimum, maximum) if (settings->name < minimum || settings->name > maximum) return 0;
    MAAGE_SHADER_CONTROLS(MAAGE_VALIDATE)
#undef MAAGE_VALIDATE
    return 1;
}
int maage_shader_settings_classify(const maage_shader_settings *settings) {
    maage_shader_settings candidate;
    int preset, equal;
    if (!settings) return 3;
    for (preset = 0; preset < 3; ++preset) {
        maage_shader_settings_preset(&candidate, preset);
        equal = 1;
#define MAAGE_COMPARE(name, minimum, maximum) if (candidate.name != settings->name) equal = 0;
        MAAGE_SHADER_CONTROLS(MAAGE_COMPARE)
#undef MAAGE_COMPARE
        if (equal) return preset;
    }
    return 3;
}
int maage_shader_setting_get(const maage_shader_settings *settings, unsigned int control) {
    if (!settings) return 0;
    switch (control) {
#define MAAGE_GET(name, minimum, maximum) case MAAGE_SHADER_##name: return settings->name;
        MAAGE_SHADER_CONTROLS(MAAGE_GET)
#undef MAAGE_GET
    default:
        return 0;
    }
}
maage_status maage_shader_setting_set(maage_shader_settings *settings, unsigned int control, int value) {
    if (!settings) return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_RENDER, MAAGE_OPERATION_SET, 0);
    switch (control) {
#define MAAGE_SET(name, minimum, maximum) case MAAGE_SHADER_##name: if (value < minimum || value > maximum) break; settings->name = value; return maage_status_make(MAAGE_STATUS_SUCCESS, 0, MAAGE_OPERATION_SET, 0);
        MAAGE_SHADER_CONTROLS(MAAGE_SET)
#undef MAAGE_SET
    default:
        break;
    }
    return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_RENDER, MAAGE_OPERATION_SET, 0);
}

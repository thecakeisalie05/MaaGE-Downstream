#version 450
#extension GL_GOOGLE_include_directive : require
#include "effects.glsl"
layout(location = 0) in vec2 maage_fragment_uv;
layout(location = 0) out vec4 result;
void main() {
    vec2 uv = maage_fragment_uv;
    vec3 eye = effect.camera_time.xyz;
    vec3 end = effect_unproject(uv, texture(opaque_depth, uv).r);
    float distance = min(length(end - eye), effect.wind.y);
    vec3 ray = normalize(end - eye);
    vec3 l = effect.sun.w >= effect.moon.w ? effect.sun.xyz : effect.moon.xyz;
    vec3 radiance = effect.sun_color.rgb * effect.sun.w + effect.moon_color.rgb * effect.moon.w;
    float phase = 0.02 + 0.08 * pow(max(dot(ray, l), 0.0), 8.0);
    float sum = 0.0;
    int steps = int(effect.quality.y);
    vec4 lightclip = effect.view_projection * vec4(eye + l * effect.wind.y, 1.0);
    vec2 lightuv = vec2(lightclip.x, -lightclip.y) / max(lightclip.w, 0.001) * 0.5 + 0.5;
    for (int i = 0; i < steps; ++i) {
        float f = (float(i) + 0.5) / float(steps);
        vec3 p = eye + ray * distance * f;
        float visibility;
        if (effect.features.x > 0.5) visibility = effect_shadow_visibility(p, -ray, 1);
        else {
            vec2 probe = mix(uv, lightuv, f);
            visibility = lightclip.w > 0.0 && all(greaterThanEqual(probe, vec2(0.0))) && all(lessThanEqual(probe, vec2(1.0))) && texture(opaque_depth, probe).r > 0.99999 ? 1.0 : 0.0;
        }
        sum += visibility * exp(-distance * f * 0.015);
    }
    result = vec4(radiance * phase * sum / max(float(steps), 1.0) * (1.0 - exp(-distance * 0.025)), texture(opaque_depth, uv).r);
}

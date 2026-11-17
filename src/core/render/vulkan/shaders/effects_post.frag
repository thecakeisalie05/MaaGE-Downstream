#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "effects.glsl"
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(location = 0) in vec2 maage_fragment_uv;
layout(location = 0) out vec4 result;
void main() {
    vec2 uv = maage_fragment_uv;
    float immersion = effect.quality.w > 0.5 ? effect.weather.w : 0.0;
    uv = clamp(uv + vec2(sin(uv.y * 25.0 + effect.camera_time.w * 1.7), cos(uv.x * 21.0 + effect.camera_time.w)) * 0.0015 * immersion, vec2(0.001), vec2(0.999));
    vec3 color = texture(scene, uv).rgb;
    float depth = texture(opaque_depth, uv).r;
    if (effect.quality.y > 0.5) {
        vec3 rays = vec3(0.0); float total = 0.0;
        for (int i = -2; i <= 2; ++i) {
            vec2 offset = vec2(i % 2, i / 2) * effect.viewport.zw * 2.0;
            vec4 sample_ray = texture(scattering, uv + offset);
            float weight = exp(-abs(sample_ray.a - depth) * 300.0);
            rays += sample_ray.rgb * weight; total += weight;
        }
        color += rays / max(total, 0.001);
    }
    if (immersion > 0.001) {
        float distance = min(80.0, length(effect_unproject(uv, depth) - effect.camera_time.xyz));
        vec3 transmittance = exp(-vec3(0.16, 0.065, 0.035) * distance * immersion);
        color = color * transmittance * (1.0 - immersion * 0.2) + vec3(0.005, 0.045, 0.065) * (1.0 - transmittance) * (0.15 + effect.sun.w * 0.5);
    }
    result = vec4(maage_shader_scene_color(color, maage.parameters.x, maage.parameters.y, true), 1.0);
}

#ifndef MAAGE_EFFECTS_GLSL
#define MAAGE_EFFECTS_GLSL
layout(set = 2, binding = 0, std140) uniform EffectFrame {
    mat4 view_projection, inverse_view_projection, shadows[4];
    vec4 camera_time, sun, moon, sun_color, moon_color;
    vec4 weather, wind, features, quality, splits, viewport, ambient;
} effect;
struct PointLight { vec4 position_radius, color_intensity; };
layout(set = 2, binding = 1, std430) readonly buffer Lights { PointLight lights[]; };
layout(set = 2, binding = 2) uniform sampler2D shadow_map;
layout(set = 2, binding = 3) uniform sampler2D opaque_depth;
layout(set = 2, binding = 4) uniform sampler2D opaque_color;
layout(set = 2, binding = 5) uniform sampler2D scattering;
vec3 effect_unproject(vec2 uv, float z) {
    vec4 p = effect.inverse_view_projection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z, 1.0);
    return p.xyz / p.w;
}
vec3 effect_wind(vec3 p, float weight, uint flags) {
    if (effect.features.z < 0.5 || (flags & 3U) == 0U) return p;
    float t = effect.camera_time.w;
    float gust = 0.65 + 0.35 * sin(t * 0.7 + p.x * 0.12 + p.z * 0.08);
    float fade = 1.0 - smoothstep(48.0, 128.0, length(p - effect.camera_time.xyz));
    float wave = sin(t * 1.9 + p.x * 0.8 + p.z * 0.6) + 0.3 * sin(t * 3.1 + p.z);
    p.xz += vec2(wave, sin(t * 1.6 + p.x * 0.65 + p.z * 0.7)) * weight * effect.wind.x * 0.2 * gust * fade * ((flags & 1U) != 0U ? 1.0 : 0.55);
    return p;
}
float effect_cascade(vec3 p, vec3 n, int cascade, int side) {
    vec4 q = effect.shadows[cascade] * vec4(p, 1.0);
    vec3 c = q.xyz / q.w;
    vec2 local = vec2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    if (any(lessThan(local, vec2(0.0))) || any(greaterThan(local, vec2(1.0))) || c.z <= 0.0 || c.z >= 1.0) return 1.0;
    vec2 grid = effect.wind.w <= 2.0 ? vec2(2.0, 1.0) : vec2(2.0);
    vec2 cell = vec2(cascade % 2, cascade / 2);
    vec2 texel = 1.0 / vec2(textureSize(shadow_map, 0));
    vec2 low = cell / grid + texel * 0.5, high = (cell + 1.0) / grid - texel * 0.5;
    vec3 l = effect.sun.w >= effect.moon.w ? effect.sun.xyz : effect.moon.xyz;
    float bias = 0.00004 + (1.0 - abs(dot(n, l))) * 0.0003;
    float result = 0.0;
    for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
        vec2 uv = clamp((local + cell) / grid + (vec2(x, y) - float(side - 1) * 0.5) * texel, low, high);
        result += c.z - bias <= texture(shadow_map, uv).r ? 1.0 : 0.0;
    }
    return result / float(side * side);
}
float effect_shadow_visibility(vec3 p, vec3 n, int side) {
    if (effect.features.x < 0.5) return 1.0;
    /* Projected W is positive forward view distance for MaaGE perspective. */
    float d = (effect.view_projection * vec4(p, 1.0)).w;
    int count = int(effect.wind.w), c = 0;
    while (c + 1 < count && d > effect.splits[c]) ++c;
    if (d > effect.splits[count - 1]) return 1.0;
    float v = effect_cascade(p, n, c, side);
    float start = c == 0 ? 0.0 : effect.splits[c - 1];
    float blend = smoothstep(mix(start, effect.splits[c], 0.85), effect.splits[c], d);
    return mix(v, c + 1 < count ? effect_cascade(p, n, c + 1, side) : 1.0, blend);
}
float effect_visibility(vec3 p, vec3 n) { return effect_shadow_visibility(p, n, int(sqrt(effect.quality.x))); }
vec3 effect_points(vec3 p, vec3 n) {
    vec3 result = vec3(0.0);
    for (int i = 0; i < int(effect.features.y); ++i) {
        vec3 delta = lights[i].position_radius.xyz - p;
        float d2 = dot(delta, delta), radius = lights[i].position_radius.w;
        float cutoff = max(0.0, 1.0 - pow(d2 / (radius * radius), 2.0));
        result += lights[i].color_intensity.rgb * lights[i].color_intensity.w * cutoff * cutoff * max(0.0, dot(n, delta * inversesqrt(max(d2, 0.0001)))) * 2.0 / (1.0 + d2);
    }
    return result;
}
vec3 effect_sky(vec3 ray) {
    if (effect.features.w < 0.5) return mix(effect.ambient.rgb / 12.92, pow((effect.ambient.rgb + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), effect.ambient.rgb));
    float horizon = pow(1.0 - max(ray.y, 0.0), 3.0);
    float mu = dot(ray, effect.sun.xyz);
    float rayleigh = 0.75 * (1.0 + mu * mu);
    float mie = 0.035 / pow(max(0.04, 1.0 + 0.76 * 0.76 - 1.52 * mu), 1.5);
    vec3 day = mix(vec3(0.10, 0.28, 0.65) * rayleigh, vec3(0.55, 0.58, 0.62), horizon);
    day += effect.sun_color.rgb * (mie + pow(max(mu, 0.0), 256.0) * 1.5) * (0.2 + horizon);
    vec3 sky = day * effect.sun.w + vec3(0.0015, 0.003, 0.009) + effect.moon_color.rgb * effect.moon.w * (0.3 + horizon);
    vec3 cell = floor(ray * 900.0);
    float star = fract(sin(dot(cell, vec3(127.1, 311.7, 74.7))) * 43758.5453);
    sky += vec3(smoothstep(0.9988, 1.0, star) * (1.0 - smoothstep(0.0, 0.15, effect.sun.w)) * max(ray.y, 0.0) * 0.6);
    return mix(sky, vec3(dot(sky, vec3(0.3, 0.5, 0.2))) * 0.65, clamp(effect.weather.x * 0.75 + effect.weather.y * 0.2, 0.0, 0.95));
}
#endif

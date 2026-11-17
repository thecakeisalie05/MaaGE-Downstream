#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "effects.glsl"
layout(location = 8) flat in uint effect_flags;
layout(location = 9) in vec3 source_position;

layout(constant_id = 1) const uint maage_linear_scene = 0;
layout(set = 0, binding = 0) uniform sampler2D maage_atlas;
layout(location = 0) in vec2 maage_fragment_uv;
layout(location = 1) in vec4 maage_fragment_color;
layout(location = 2) in vec2 maage_fragment_light;
layout(location = 3) flat in float maage_fragment_material;
layout(location = 4) in vec3 maage_fog_position;
layout(location = 5) flat in vec4 maage_tile;
layout(location = 6) in vec3 maage_world_position;
layout(location = 7) flat in uint maage_rotation_face;
layout(location = 0) out vec4 maage_result;

void main()
{
    vec4 color;
    if (maage_fragment_material == float(MAAGE_MESH_MATERIAL_STARFIELD)) {
        color = maage_shader_starfield(maage_world_position, maage_fog_position, maage.matrix, maage.parameters.x);
    } else {
        vec4 texel = texture(maage_atlas, maage_shader_tile_uv(maage_fragment_uv, maage_tile, source_position, maage_rotation_face));
        if (maage_shader_cutout(maage_fragment_material, texel.a)) discard;
        color = maage_shader_mesh_color(texel, maage_fragment_color, maage_fragment_light, maage_fragment_material, maage.lighting, maage.parameters);
    }
    vec3 n = normalize(cross(dFdx(maage_world_position), dFdy(maage_world_position)));
    vec3 view = normalize(effect.camera_time.xyz - maage_world_position);
    if (dot(n, view) < 0.0) n = -n;
    vec3 sunlit = effect.sun_color.rgb * effect.sun.w * max(dot(n, effect.sun.xyz), 0.0) + effect.moon_color.rgb * effect.moon.w * max(dot(n, effect.moon.xyz), 0.0);
    float visibility = effect_visibility(maage_world_position, n);
    if (maage_fragment_material != float(MAAGE_MESH_MATERIAL_MULTIPLY) && maage_fragment_material != float(MAAGE_MESH_MATERIAL_STARFIELD)) {
        vec3 linear = maage_shader_srgb_decode(color.rgb);
        float sky = clamp(maage_fragment_light.x, 0.0, 1.0);
        float baked = clamp(maage_fragment_light.y, 0.0, 1.0);
        vec3 albedo = maage_shader_srgb_decode(texture(maage_atlas, maage_shader_tile_uv(maage_fragment_uv, maage_tile, source_position, maage_rotation_face)).rgb * maage_fragment_color.rgb);
        vec3 points = max(effect_points(maage_world_position, n) - vec3(baked * baked * 0.65), vec3(0.0));
        if (effect.weather.z > 0.5) linear *= mix(1.0, 0.40 + 0.60 * visibility, sky * (1.0 - baked) * clamp(effect.sun.w + effect.moon.w * 4.0, 0.0, 1.0));
        linear += albedo * (sunlit * sky * visibility * 0.4 + points);
        color.rgb = maage_shader_srgb_encode(linear);
    }
    vec4 fog = maage.fog;
    if (maage_linear_scene != 0) {
        /* Multiplicative shadows carry a factor, not radiance. Their neutral
         * 0.5 remains neutral under the shared two-source-color blend rule. */
        if (maage_fragment_material != float(MAAGE_MESH_MATERIAL_MULTIPLY)) color.rgb = maage_shader_srgb_decode(color.rgb);
        fog.rgb = maage_shader_srgb_decode(fog.rgb);
    }
    if (effect.features.w > 0.5) fog.rgb = effect_sky(normalize(maage_world_position - effect.camera_time.xyz));
    if ((effect_flags & 4U) != 0U && effect.wind.z > 0.5) {
        vec2 screen = gl_FragCoord.xy * effect.viewport.zw;
        float t = effect.camera_time.w;
        vec3 wave = normalize(n + vec3(sin(maage_world_position.x * 1.6 + t * 1.3) * 0.09, 0.0, cos(maage_world_position.z * 1.4 - t) * 0.09));
        vec2 refract_uv = clamp(screen + wave.xz * 0.012, vec2(0.001), vec2(0.999));
        float z = texture(opaque_depth, refract_uv).r;
        if (z < gl_FragCoord.z) refract_uv = screen;
        z = texture(opaque_depth, refract_uv).r;
        float thickness = min(32.0, length(effect_unproject(refract_uv, z) - maage_world_position));
        vec3 transmission = exp(-vec3(0.18, 0.065, 0.035) * thickness);
        vec3 water = mix(vec3(0.015, 0.095, 0.11) * (0.3 + effect.sun.w), texture(opaque_color, refract_uv).rgb, transmission);
        vec3 reflected = reflect(-view, wave);
        vec3 reflection = effect_sky(reflected);
        if (effect.quality.z > 0.5) {
            int steps = effect.quality.z > 1.5 ? 16 : 8;
            for (int i = 1; i <= steps; ++i) {
                vec3 q = maage_world_position + reflected * (0.4 + float(i * i) * 0.12);
                vec4 clip = effect.view_projection * vec4(q, 1.0);
                vec2 uv = vec2(clip.x, -clip.y) / clip.w * 0.5 + 0.5;
                if (clip.w <= 0.0 || any(lessThan(uv, vec2(0.01))) || any(greaterThan(uv, vec2(0.99)))) break;
                float d = texture(opaque_depth, uv).r;
                vec3 hit = effect_unproject(uv, d);
                if (d < 0.99999 && clip.z / clip.w > d && length(hit - q) < 1.5) {
                    float edge = smoothstep(0.0, 0.08, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
                    reflection = mix(reflection, texture(opaque_color, uv).rgb, edge); break;
                }
            }
        }
        float fresnel = 0.02 + 0.98 * pow(1.0 - max(dot(wave, view), 0.0), 5.0);
        vec3 specular = effect.sun_color.rgb * effect.sun.w * pow(max(dot(reflect(-effect.sun.xyz, wave), view), 0.0), 96.0) + effect.moon_color.rgb * effect.moon.w * pow(max(dot(reflect(-effect.moon.xyz, wave), view), 0.0), 96.0);
        color.rgb = mix(water, reflection, fresnel) + specular * visibility + effect_points(maage_world_position, wave) * 0.08;
        color.a = clamp(0.55 + fresnel * 0.4 + thickness * 0.005, 0.55, 0.95);
    }
    maage_result = maage_shader_fog(color, maage_fog_position, maage_fragment_material, fog, maage.fog_distance, maage.lighting);
}

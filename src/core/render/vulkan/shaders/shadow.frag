#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(set = 0, binding = 0) uniform sampler2D maage_atlas;
layout(location = 0) in vec2 maage_fragment_uv;
layout(location = 3) flat in float maage_fragment_material;
layout(location = 5) flat in vec4 maage_tile;
layout(location = 6) in vec3 maage_world_position;
layout(location = 7) flat in uint maage_rotation_face;
void main() {
    float alpha = texture(maage_atlas, maage_shader_tile_uv(maage_fragment_uv, maage_tile, maage_world_position, maage_rotation_face)).a;
    if (maage_shader_cutout(maage_fragment_material, alpha)) discard;
}

#version 450
#extension GL_GOOGLE_include_directive : require
#include "effects.glsl"
layout(location = 0) in vec2 maage_fragment_uv;
layout(location = 0) out vec4 result;
void main() { result = vec4(effect_sky(normalize(effect_unproject(maage_fragment_uv, 1.0) - effect.camera_time.xyz)), 1.0); }

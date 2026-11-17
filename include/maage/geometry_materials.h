#ifndef MAAGE_GEOMETRY_MATERIALS_H
#define MAAGE_GEOMETRY_MATERIALS_H
/* Numeric surface behavior, constant across a triangle. Translucent surfaces
 * blend after solid/cutout geometry with depth testing and no depth writes.
 * Shared by portable geometry and both native shader languages. */
#define MAAGE_MESH_MATERIAL_OPAQUE 0
#define MAAGE_MESH_MATERIAL_CUTOUT 1
#define MAAGE_MESH_MATERIAL_TRANSLUCENT 2
#define MAAGE_MESH_MATERIAL_MULTIPLY 3
#define MAAGE_MESH_MATERIAL_STARFIELD 4
#define MAAGE_MESH_MATERIAL_COUNT 5
#define MAAGE_MESH_ROTATION_FACE_COUNT 6
/* Upper rotation metadata bits are presentation-only; vertex/surface strides stay fixed. */
#define MAAGE_MESH_EFFECT_ROOTED 1U
#define MAAGE_MESH_EFFECT_FOLIAGE 2U
#define MAAGE_MESH_EFFECT_WATER 4U
#define MAAGE_MESH_EFFECT_MASK 7U
#define MAAGE_MESH_EFFECT_SHIFT 3U
#endif

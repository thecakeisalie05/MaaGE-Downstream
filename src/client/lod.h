#ifndef MAAGE_CLIENT_LOD_H
#define MAAGE_CLIENT_LOD_H
#include "maage/render.h"
#include "maage/world.h"
#include "maage/biome.h"
#include <stddef.h>

/* Presentation-only decimated surface meshes for resident World columns.
 * No LOD state participates in collision, lighting, persistence or simulation.
 * Uploads borrow CPU geometry until completion. */
typedef struct maage_client_lod_mesh {
    maage_render_mesh *gpu;
    unsigned long revision;
    int quality;
} maage_client_lod_mesh;

typedef struct maage_client_lod_pending {
    maage_render_mesh *gpu;
    maage_mesh_vertex *vertices;
    unsigned int *indices;
    size_t slot;
    unsigned long revision;
    int quality;
} maage_client_lod_pending;

typedef struct maage_client_lod {
    maage_client_lod_pending pending;
    size_t cursor;
    unsigned long completed;
} maage_client_lod;

typedef struct maage_client_chunk_mesh maage_client_chunk_mesh;
/* Source is the resident renderer-owned mesh slot; no new World ownership. */
typedef struct maage_client_lod_source {
    int x, z, present;
    unsigned long revision;
    maage_client_lod_mesh *mesh;
} maage_client_lod_source;

void maage_client_lod_cancel(maage_client_lod *lod);
void maage_client_lod_clear(maage_client_lod *lod);
void maage_client_lod_mesh_clear(maage_client_lod_mesh *mesh);
/* At most one new column build or one bounded upload step per call.
 * Caller supplies stable slot identities for the duration of this call.
 * Failures preserve the full-resolution fallback and cancel the candidate. */
maage_status maage_client_lod_step(maage_client_lod *lod, maage_world *world,
    maage_renderer *renderer, maage_render_texture *atlas, const maage_biome_registry *biomes,
    maage_client_chunk_mesh *slots, size_t count,
    int center_x, int center_z, int detail_radius, int far_radius, int quality);
#endif

#ifndef MAAGE_CLIENT_DISTANT_LOD_H
#define MAAGE_CLIENT_DISTANT_LOD_H
#include "maage/base.h"
#include "maage/render.h"
#include "maage/world.h"
#include <stddef.h>
/* Presentation-only quadtree. Worker-owned sampling, derivative I/O and CPU
 * preparation; owner publishes bounded GPU uploads. Never simulates. */
typedef struct maage_distant_lod maage_distant_lod;
typedef struct maage_distant_lod_stats {
    unsigned long published, cache_hits, generated, stale;
    unsigned active, publication_backlog, resident, by_level[5];
    size_t gpu_bytes_estimate;
    double generation_ms; /* Worker wall time including disk, never GPU timing. */
    int budget_limited;
} maage_distant_lod_stats;
maage_distant_lod *maage_distant_lod_create(const char *instance_directory, const char world_id[37], maage_dimension_id dimension, maage_world_seed seed, const maage_generation_policy *policy, const maage_block_registry *blocks, const maage_biome_registry *biomes);
/* Lifecycle only. Invalidate GPU on its owner before background CPU cleanup. */
maage_status maage_distant_lod_destroy(maage_distant_lod *lod);
void maage_distant_lod_invalidate_gpu(maage_distant_lod *lod);
maage_status maage_distant_lod_set_appearance(maage_distant_lod *lod, const maage_block_registry *blocks, const maage_biome_registry *biomes);
size_t maage_distant_lod_capacity(const maage_distant_lod *lod);
int maage_distant_lod_ready(const maage_distant_lod *lod, int x, int z, int quality);
void maage_distant_lod_suppress(maage_distant_lod *lod, int x, int z);
/* SUCCESS accepted a confirmed snapshot; NOOP applies backpressure. */
maage_status maage_distant_lod_observe(maage_distant_lod *lod, const maage_world *world, int x, int z);
maage_status maage_distant_lod_step(maage_distant_lod *lod, maage_renderer *renderer, maage_render_texture *atlas, int center_x, int center_z, int near_radius, int far_radius, int quality, double yaw);
size_t maage_distant_lod_append(const maage_distant_lod *lod, const maage_render_mesh **draws, size_t capacity, const float matrix[16]);
void maage_distant_lod_get_stats(const maage_distant_lod *lod, maage_distant_lod_stats *stats);
#endif

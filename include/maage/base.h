#ifndef MAAGE_BASE_H
#define MAAGE_BASE_H
#include "maage/content.h"
#include "maage/world.h"
/* Statically linked minecraft root module. Registration performs no window,
 * device or asset I/O. Applications explicitly compose this with other modules. */
extern const maage_content_module maage_base_module;

/* Procedural terrain-only query. No World, chunk, meshing job, gameplay
 * simulation or GPU resources are created. The caller owns the sampler;
 * one sampler must not be shared concurrently across threads. */
typedef struct maage_base_lod_sampler maage_base_lod_sampler;
typedef struct maage_base_lod_column {
    int height, water_height, canopy_height; /* Upper surface planes; zero water/canopy means absent. */
    maage_block_state_id canopy;
    maage_block_state_id surface, water;
    maage_biome_id biome;
} maage_base_lod_column;
maage_base_lod_sampler *maage_base_lod_sampler_create(
    const maage_block_registry *blocks, const maage_biome_registry *biomes,
    maage_world_seed seed, const maage_generation_policy *policy);
int maage_base_lod_sampler_sample(maage_base_lod_sampler *sampler,
    int x, int z, maage_base_lod_column *column);
void maage_base_lod_sampler_destroy(maage_base_lod_sampler *sampler);
#endif

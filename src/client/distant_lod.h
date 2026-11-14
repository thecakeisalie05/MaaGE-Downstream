#ifndef MAAGE_CLIENT_DISTANT_LOD_H
#define MAAGE_CLIENT_DISTANT_LOD_H
#include "maage/base.h"
#include "maage/render.h"
#include "maage/world.h"
#include <stddef.h>
#define MAAGE_DISTANT_MAX_TILES 1200U
/* Independent distant terrain renderer, versioned world-specific disk cache.
 * Terrain data is generated directly from seeded Base terrain, not streamed
 * World columns. Cached geometry is presentation-only. */
typedef struct maage_distant_lod maage_distant_lod;
maage_distant_lod *maage_distant_lod_create(
    const char *instance_directory, const char world_id[37],
    maage_world_seed seed, const maage_generation_policy *policy,
    const maage_block_registry *blocks, const maage_biome_registry *biomes);
void maage_distant_lod_destroy(maage_distant_lod *lod);
void maage_distant_lod_invalidate_gpu(maage_distant_lod *lod);
int maage_distant_lod_ready(const maage_distant_lod *lod, int x, int z, int quality);
maage_status maage_distant_lod_step(maage_distant_lod *lod,
    maage_renderer *renderer, maage_render_texture *atlas,
    int center_x, int center_z, int near_radius, int far_radius, int quality);
size_t maage_distant_lod_append(const maage_distant_lod *lod,
    const maage_render_mesh **draws, size_t capacity,
    int center_x, int center_z, int near_radius, int far_radius, int quality);
#endif

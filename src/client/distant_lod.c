#include "distant_lod.h"
#include "lod.h"
#include "maage/storage.h"
#include "maage/geometry.h"
#include "maage/geometry_materials.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#define MAAGE_DISTANT_DISK_MAGIC UINT32_C(0x4d474c44)
#define MAAGE_DISTANT_DISK_VERSION 1U

typedef struct maage_distant_disk {
    uint32_t magic, version, seed_hi, seed_lo;
    int32_t x, z, sea_level;
    uint32_t checksum;
    int32_t heights[256];
    uint32_t surface[256], biome[256];
} maage_distant_disk;
typedef struct maage_distant_tile {
    int x, z, used, quality;
    maage_render_mesh *gpu;
} maage_distant_tile;
struct maage_distant_lod {
    char directory[MAAGE_PATH_CAPACITY];
    maage_base_lod_sampler *sampler;
    const maage_block_registry *blocks;
    const maage_biome_registry *biomes;
    maage_world_seed seed;
    maage_generation_policy policy;
    maage_distant_tile *tiles;
    size_t cursor;
    maage_render_mesh *pending_gpu;
    maage_mesh_vertex *pending_vertices;
    unsigned int *pending_indices;
    size_t pending_index;
    int pending_quality;
    unsigned long completed, disk_hits, generated;
};
typedef struct maage_lod_surface {
    int ground, water, present;
    const maage_block_state_definition *ground_state, *water_state;
    const maage_biome_definition *biome;
} maage_lod_surface;

static void lod_write_face(maage_mesh_vertex *vertices, unsigned int *indices, size_t *faces,
    const maage_block_state_definition *state, const maage_biome_definition *biome,
    unsigned int face, float x0, float z0, float x1, float z1, float bottom, float top, int fluid) {
    static const unsigned int triangles[6] = {0U, 1U, 2U, 0U, 2U, 3U};
    const maage_block_face_definition *definition = &state->faces[face];
    const float *uv = definition->uv;
    unsigned int k, component;
    float point[4][3], u, v, tint, shade;
    size_t offset = *faces * 4U;
    size_t elements = *faces * 6U;
    if (face == MAAGE_FACE_POSITIVE_Y) {
        point[0][0] = x0; point[0][1] = top; point[0][2] = z1;
        point[1][0] = x1; point[1][1] = top; point[1][2] = z1;
        point[2][0] = x1; point[2][1] = top; point[2][2] = z0;
        point[3][0] = x0; point[3][1] = top; point[3][2] = z0;
    } else if (face == MAAGE_FACE_NEGATIVE_X) {
        point[0][0] = x0; point[0][1] = bottom; point[0][2] = z0;
        point[1][0] = x0; point[1][1] = bottom; point[1][2] = z1;
        point[2][0] = x0; point[2][1] = top; point[2][2] = z1;
        point[3][0] = x0; point[3][1] = top; point[3][2] = z0;
    } else if (face == MAAGE_FACE_POSITIVE_X) {
        point[0][0] = x1; point[0][1] = bottom; point[0][2] = z1;
        point[1][0] = x1; point[1][1] = bottom; point[1][2] = z0;
        point[2][0] = x1; point[2][1] = top; point[2][2] = z0;
        point[3][0] = x1; point[3][1] = top; point[3][2] = z1;
    } else if (face == MAAGE_FACE_NEGATIVE_Z) {
        point[0][0] = x1; point[0][1] = bottom; point[0][2] = z0;
        point[1][0] = x0; point[1][1] = bottom; point[1][2] = z0;
        point[2][0] = x0; point[2][1] = top; point[2][2] = z0;
        point[3][0] = x1; point[3][1] = top; point[3][2] = z0;
    } else {
        point[0][0] = x0; point[0][1] = bottom; point[0][2] = z1;
        point[1][0] = x1; point[1][1] = bottom; point[1][2] = z1;
        point[2][0] = x1; point[2][1] = top; point[2][2] = z1;
        point[3][0] = x0; point[3][1] = top; point[3][2] = z1;
    }
    shade = face == MAAGE_FACE_POSITIVE_Y ? 1.0f : (face == MAAGE_FACE_NEGATIVE_X || face == MAAGE_FACE_POSITIVE_X ? 0.6f : 0.8f);
    for (k = 0; k < 4U; ++k) {
        maage_mesh_vertex *vertex = &vertices[offset + k];
        u = (k == 1U || k == 2U) ? 1.0f : 0.0f;
        v = k < 2U ? 1.0f : 0.0f;
        vertex->position[0] = point[k][0];
        vertex->position[1] = point[k][1];
        vertex->position[2] = point[k][2];
        /* Absolute atlas UVs with a zero tile span. The shader's tiled mode
         * wraps UV=1 to zero and reduces the whole face to a single texel. */
        vertex->uv[0] = uv[0] + u * (uv[2] - uv[0]);
        vertex->uv[1] = uv[1] + v * (uv[3] - uv[1]);
        for (component = 0; component < 3U; ++component) {
            tint = definition->tint[component];
            if (biome && (definition->tint_mode == MAAGE_BLOCK_TINT_GRASS ||
                definition->tint_mode == MAAGE_BLOCK_TINT_GRASS_LOCAL))
                tint *= biome->grass_color[component];
            else if (biome && definition->tint_mode == MAAGE_BLOCK_TINT_FOLIAGE)
                tint *= biome->foliage_color[component];
            vertex->color[component] = tint * shade;
            vertex->animation[component] = definition->animation[component];
        }
        vertex->color[3] = fluid ? definition->tint[3] : 1.0f;
        vertex->light[0] = 1.0f;
        vertex->light[1] = 0.0f;
        vertex->material = fluid ? MAAGE_MESH_MATERIAL_TRANSLUCENT : MAAGE_MESH_MATERIAL_OPAQUE;
    }
    for (k = 0; k < 6U; ++k) indices[elements + k] = (unsigned int) offset + triangles[k];
    ++*faces;
}


static int lod_absmax(int a, int b) { return a > b ? a : b; }
static int lod_distance(int x, int z, int cx, int cz) {
    return lod_absmax(abs(x - cx), abs(z - cz));
}
static int lod_disk_path(const maage_distant_lod *lod, int x, int z,
    char path[MAAGE_PATH_CAPACITY], int temporary) {
    int length = snprintf(path, MAAGE_PATH_CAPACITY, "%s/%d_%d.mgdl%s",
        lod->directory, x, z, temporary ? ".tmp" : "");
    return length > 0 && length < MAAGE_PATH_CAPACITY;
}
static uint32_t lod_disk_checksum(const maage_distant_disk *record) {
    const unsigned char *bytes = (const unsigned char *) record->heights;
    size_t i, length = sizeof(*record) - offsetof(maage_distant_disk, heights);
    uint32_t hash = UINT32_C(2166136261);
    for (i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= UINT32_C(16777619);
    }
    return hash;
}
static int lod_disk_load(maage_distant_lod *lod, int x, int z, maage_distant_disk *record) {
    FILE *file;
    char path[MAAGE_PATH_CAPACITY];
    size_t length, i;
    if (!lod_disk_path(lod, x, z, path, 0) || maage_storage_status(path) != 1) return 0;
    file = maage_storage_open(path, "rb");
    if (!file) return 0;
    length = fread(record, 1, sizeof(*record), file);
    if (fgetc(file) != EOF) length = 0;
    fclose(file);
    if (length != sizeof(*record) ||
        record->magic != MAAGE_DISTANT_DISK_MAGIC ||
        record->version != MAAGE_DISTANT_DISK_VERSION ||
        record->seed_hi != (uint32_t) lod->seed.high ||
        record->seed_lo != (uint32_t) lod->seed.low ||
        record->sea_level != lod->policy.sea_level ||
        record->x != x || record->z != z ||
        record->checksum != lod_disk_checksum(record)) return 0;
    for (i = 0; i < 256U; ++i)
        if (record->heights[i] < 0 || record->heights[i] > MAAGE_WORLD_HEIGHT ||
            !maage_block_registry_state(lod->blocks, record->surface[i]) ||
            !maage_biome_registry_get(lod->biomes, record->biome[i])) return 0;
    ++lod->disk_hits;
    return 1;
}
static void lod_disk_save(maage_distant_lod *lod, const maage_distant_disk *record) {
    char path[MAAGE_PATH_CAPACITY], temporary[MAAGE_PATH_CAPACITY];
    FILE *file;
    if (!lod_disk_path(lod, record->x, record->z, path, 0) ||
        !lod_disk_path(lod, record->x, record->z, temporary, 1)) return;
    file = maage_storage_open(temporary, "wb");
    if (!file) return;
    if (fwrite(record, 1, sizeof(*record), file) != sizeof(*record) ||
        maage_status_failed(maage_storage_sync(file))) {
        fclose(file);
        (void) maage_storage_remove(temporary);
        return;
    }
    if (fclose(file) != 0 || maage_status_failed(maage_storage_replace(temporary, path)))
        (void) maage_storage_remove(temporary);
}
static int lod_disk_generate(maage_distant_lod *lod, int cx, int cz,
    maage_distant_disk *record) {
    int x, z;
    maage_base_lod_column sample;
    memset(record, 0, sizeof(*record));
    record->magic = MAAGE_DISTANT_DISK_MAGIC;
    record->version = MAAGE_DISTANT_DISK_VERSION;
    record->seed_hi = (uint32_t) lod->seed.high;
    record->seed_lo = (uint32_t) lod->seed.low;
    record->x = cx; record->z = cz;
    record->sea_level = lod->policy.sea_level;
    for (z = 0; z < 16; ++z) for (x = 0; x < 16; ++x) {
        if (!maage_base_lod_sampler_sample(lod->sampler,
                cx * 16 + x, cz * 16 + z, &sample)) return 0;
        record->heights[z * 16 + x] = sample.height;
        if (sample.surface > UINT32_MAX || sample.biome > UINT32_MAX) return 0;
        record->surface[z * 16 + x] = (uint32_t) sample.surface;
        record->biome[z * 16 + x] = (uint32_t) sample.biome;
    }
    record->checksum = lod_disk_checksum(record);
    ++lod->generated;
    lod_disk_save(lod, record);
    return 1;
}
static int lod_sample(maage_distant_lod *lod, const maage_distant_disk *record,
    int x, int z, maage_lod_surface *surface) {
    maage_base_lod_column sample;
    int local_x = x - record->x * 16, local_z = z - record->z * 16;
    memset(surface, 0, sizeof(*surface));
    if (local_x >= 0 && local_x < 16 && local_z >= 0 && local_z < 16) {
        int index = local_z * 16 + local_x;
        sample.height = record->heights[index];
        sample.surface = record->surface[index];
        sample.biome = record->biome[index];
        sample.water_height = sample.height < lod->policy.sea_level ? lod->policy.sea_level : 0;
        /* Use current registered water state; no block ID from a stale cache. */
        sample.water = 0;
    } else if (!maage_base_lod_sampler_sample(lod->sampler, x, z, &sample)) return 0;
    surface->ground = sample.height;
    surface->water = sample.water_height;
    surface->ground_state = maage_block_registry_state(lod->blocks, sample.surface);
    surface->biome = maage_biome_registry_get(lod->biomes, sample.biome);
    /* The generated water material comes from the same Base generator. */
    if (surface->water > surface->ground) {
        maage_base_lod_column water_sample;
        if (maage_base_lod_sampler_sample(lod->sampler, x, z, &water_sample))
            surface->water_state = maage_block_registry_state(lod->blocks, water_sample.water);
    }
    surface->present = surface->ground_state != NULL;
    return surface->present;
}
static maage_status lod_build(maage_distant_lod *lod, int cx, int cz, int quality,
    maage_mesh_vertex **vertices, unsigned int **indices, maage_mesh_data *data) {
    maage_distant_disk record;
    maage_lod_surface samples[18][18], *center, *neighbor;
    maage_mesh_vertex *v;
    unsigned int *ix, step, patches, x, z, side;
    int wx, wz, neighbor_heights[4];
    float x0, x1, z0, z1;
    size_t count = 0, capacity;
    static const int dx[4] = {-1, 1, 0, 0}, dz[4] = {0, 0, -1, 1};
    static const unsigned int faces[4] = {
        MAAGE_FACE_NEGATIVE_X, MAAGE_FACE_POSITIVE_X, MAAGE_FACE_NEGATIVE_Z, MAAGE_FACE_POSITIVE_Z
    };
    if (!lod_disk_load(lod, cx, cz, &record) &&
        !lod_disk_generate(lod, cx, cz, &record))
        return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_CREATE, 0);
    *vertices = NULL;
    *indices = NULL;
    memset(data, 0, sizeof(*data));
    step = quality == 0 ? 1U : (quality == 1 ? 2U : 4U);
    patches = 16U / step;
    capacity = (size_t) patches * patches * 10U;
    v = (maage_mesh_vertex *) calloc(capacity * 4U, sizeof(*v));
    ix = (unsigned int *) malloc(capacity * 6U * sizeof(*ix));
    if (!v || !ix) {
        free(v); free(ix);
        return maage_status_make(MAAGE_STATUS_ALLOCATION_FAILED, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_CREATE, 0);
    }
    memset(samples, 0, sizeof(samples));
    for (z = 0; z < patches + 2U; ++z) for (x = 0; x < patches + 2U; ++x) {
        wx = cx * 16 + ((int) x - 1) * (int) step + (int) step / 2;
        wz = cz * 16 + ((int) z - 1) * (int) step + (int) step / 2;
        (void) lod_sample(lod, &record, wx, wz, &samples[z][x]);
    }
    for (z = 1U; z <= patches; ++z) for (x = 1U; x <= patches; ++x) {
        center = &samples[z][x];
        if (!center->ground_state) continue;
        x0 = (float) (cx * 16 + ((int) x - 1) * (int) step);
        z0 = (float) (cz * 16 + ((int) z - 1) * (int) step);
        x1 = x0 + (float) step; z1 = z0 + (float) step;
        lod_write_face(v, ix, &count, center->ground_state, center->biome, MAAGE_FACE_POSITIVE_Y,
            x0, z0, x1, z1, 0.0f, (float) center->ground, 0);
        for (side = 0U; side < 4U; ++side) {
            neighbor = &samples[(int) z + dz[side]][(int) x + dx[side]];
            neighbor_heights[side] = neighbor->present ? neighbor->ground : center->ground;
            if (neighbor_heights[side] < center->ground)
                lod_write_face(v, ix, &count, center->ground_state, center->biome, faces[side],
                    x0, z0, x1, z1, (float) neighbor_heights[side], (float) center->ground, 0);
        }
        if (center->water_state && center->water > center->ground) {
            lod_write_face(v, ix, &count, center->water_state, center->biome, MAAGE_FACE_POSITIVE_Y,
                x0, z0, x1, z1, 0.0f, (float) center->water - 0.10f, 1);
            for (side = 0U; side < 4U; ++side) {
                int floor;
                neighbor = &samples[(int) z + dz[side]][(int) x + dx[side]];
                if (!neighbor->present) continue;
                floor = neighbor->ground;
                if (neighbor->water_state && neighbor->water > floor) floor = neighbor->water;
                if (floor < center->ground) floor = center->ground;
                if (floor < center->water)
                    lod_write_face(v, ix, &count, center->water_state, center->biome, faces[side],
                        x0, z0, x1, z1, (float) floor, (float) center->water - 0.10f, 1);
            }
        }
    }
    if (!count) {
        free(v); free(ix);
        return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_CREATE, 0);
    }
    data->vertices = v; data->vertex_count = count * 4U;
    data->indices = ix; data->index_count = count * 6U;
    *vertices = v; *indices = ix;
    return maage_status_make(MAAGE_STATUS_SUCCESS, 0, MAAGE_OPERATION_CREATE, 0);
}
static void lod_cancel_pending(maage_distant_lod *lod) {
    maage_render_mesh_destroy(lod->pending_gpu);
    free(lod->pending_vertices);
    free(lod->pending_indices);
    lod->pending_gpu = NULL;
    lod->pending_vertices = NULL;
    lod->pending_indices = NULL;
}
maage_distant_lod *maage_distant_lod_create(
    const char *instance_directory, const char world_id[37],
    maage_dimension_id dimension, maage_world_seed seed, const maage_generation_policy *policy,
    const maage_block_registry *blocks, const maage_biome_registry *biomes) {
    maage_distant_lod *lod;
    char relative[128];
    if (!instance_directory || !world_id || !maage_storage_uuid_valid(world_id) ||
        !dimension || !blocks || !biomes || !policy) return NULL;
    lod = (maage_distant_lod *) calloc(1, sizeof(*lod));
    if (!lod) return NULL;
    lod->tiles = (maage_distant_tile *) calloc(MAAGE_DISTANT_MAX_TILES, sizeof(*lod->tiles));
    lod->sampler = maage_base_lod_sampler_create(blocks, biomes, seed, policy);
    if (!lod->tiles || !lod->sampler) goto failed;
    lod->seed = seed; lod->policy = *policy;
    lod->blocks = blocks; lod->biomes = biomes;
    snprintf(relative, sizeof(relative), "worlds/%s/lod-cache/v2/d%lu", world_id,
        (unsigned long) dimension);
    if (maage_status_failed(maage_storage_join(lod->directory,
            sizeof(lod->directory), instance_directory, relative)) ||
        maage_status_failed(maage_storage_directories(lod->directory))) goto failed;
    return lod;
failed:
    maage_distant_lod_destroy(lod);
    return NULL;
}
void maage_distant_lod_invalidate_gpu(maage_distant_lod *lod) {
    size_t i;
    if (!lod) return;
    lod_cancel_pending(lod);
    for (i = 0; i < MAAGE_DISTANT_MAX_TILES; ++i) {
        maage_render_mesh_destroy(lod->tiles[i].gpu);
        memset(&lod->tiles[i], 0, sizeof(lod->tiles[i]));
    }
}
void maage_distant_lod_destroy(maage_distant_lod *lod) {
    if (!lod) return;
    maage_distant_lod_invalidate_gpu(lod);
    maage_base_lod_sampler_destroy(lod->sampler);
    free(lod->tiles);
    free(lod);
}
static maage_distant_tile *lod_tile(maage_distant_lod *lod, int x, int z) {
    size_t i;
    for (i = 0; i < MAAGE_DISTANT_MAX_TILES; ++i)
        if (lod->tiles[i].used && lod->tiles[i].x == x && lod->tiles[i].z == z)
            return &lod->tiles[i];
    return NULL;
}
int maage_distant_lod_ready(const maage_distant_lod *lod, int x, int z, int quality) {
    size_t i;
    if (!lod) return 0;
    for (i = 0; i < MAAGE_DISTANT_MAX_TILES; ++i)
        if (lod->tiles[i].used && lod->tiles[i].x == x && lod->tiles[i].z == z &&
            lod->tiles[i].gpu && lod->tiles[i].quality == quality) return 1;
    return 0;
}
maage_status maage_distant_lod_step(maage_distant_lod *lod,
    maage_renderer *renderer, maage_render_texture *atlas,
    int center_x, int center_z, int near_radius, int far_radius, int quality) {
    maage_mesh_data data;
    maage_mesh_preparation *prepared = NULL;
    maage_status result;
    maage_distant_tile *tile = NULL;
    size_t i, free_slot = MAAGE_DISTANT_MAX_TILES;
    int x, z, distance, target, ring, offset, side;
    if (!lod || !renderer || !atlas || quality < 0 || quality > 2)
        return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_UPDATE, 0);
    if (lod->pending_gpu) {
        result = maage_render_mesh_upload_step(lod->pending_gpu, 262144U, NULL);
        if (maage_status_code(result) == MAAGE_STATUS_NOOP) return result;
        if (maage_status_failed(result)) {
            lod_cancel_pending(lod);
            return result;
        }
        tile = &lod->tiles[lod->pending_index];
        maage_render_mesh_destroy(tile->gpu);
        tile->gpu = lod->pending_gpu;
        tile->quality = lod->pending_quality;
        lod->pending_gpu = NULL;
        free(lod->pending_vertices); lod->pending_vertices = NULL;
        free(lod->pending_indices); lod->pending_indices = NULL;
        ++lod->completed;
        return result;
    }
    if (far_radius <= near_radius + 2) return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_UPDATE, 0);
    for (i = 0; i < MAAGE_DISTANT_MAX_TILES; ++i) {
        tile = &lod->tiles[i];
        if (tile->used && lod_distance(tile->x, tile->z, center_x, center_z) > far_radius + 1) {
            maage_render_mesh_destroy(tile->gpu);
            memset(tile, 0, sizeof(*tile));
        }
        if (!tile->used && free_slot == MAAGE_DISTANT_MAX_TILES) free_slot = i;
    }
    /* Search rings near-first. One tile build or bounded upload step/frame. */
    for (ring = near_radius + 3; ring <= far_radius; ++ring)
        for (side = -ring; side <= ring; ++side)
            for (offset = 0; offset < 4; ++offset) {
                x = center_x + (offset == 0 ? -ring : offset == 1 ? ring : side);
                z = center_z + (offset == 2 ? -ring : offset == 3 ? ring : side);
                if (x < -MAAGE_WORLD_LIMIT / 16 || x > MAAGE_WORLD_LIMIT / 16 ||
                    z < -MAAGE_WORLD_LIMIT / 16 || z > MAAGE_WORLD_LIMIT / 16) continue;
                distance = lod_distance(x, z, center_x, center_z);
                target = maage_client_lod_quality_at(x, z, center_x, center_z, near_radius, far_radius, quality);
                if (distance <= near_radius + 2 || target < 0) continue;
                tile = lod_tile(lod, x, z);
                if (tile && (tile->quality == -2 || (tile->gpu && tile->quality == target))) continue;
                if (!tile) {
                    if (free_slot == MAAGE_DISTANT_MAX_TILES) continue;
                    tile = &lod->tiles[free_slot];
                    tile->x = x; tile->z = z; tile->used = 1;
                }
                /* Retain the old tile while its replacement uploads; the
                 * renderer will keep drawing it rather than opening a hole. */
                result = lod_build(lod, x, z, target, &lod->pending_vertices, &lod->pending_indices, &data);
                if (maage_status_code(result) == MAAGE_STATUS_NOOP) {
                    tile->quality = -2; /* Bad/unsupported source: do not stall every frame. */
                    return result;
                }
                if (maage_status_failed(result)) {
                    if (!tile->gpu) tile->used = 0;
                    return result;
                }
                result = maage_mesh_prepare(&data, &prepared);
                if (!maage_status_failed(result))
                    result = maage_renderer_begin_geometry(renderer, &prepared, atlas, &lod->pending_gpu, NULL);
                maage_mesh_preparation_destroy(prepared);
                if (maage_status_failed(result)) {
                    lod_cancel_pending(lod);
                    if (!tile->gpu) tile->used = 0;
                    return result;
                }
                lod->pending_index = (size_t) (tile - lod->tiles);
                lod->pending_quality = target;
                return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_UPDATE, 0);
            }
    return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_UPDATE, 0);
}
size_t maage_distant_lod_append(const maage_distant_lod *lod,
    const maage_render_mesh **draws, size_t capacity,
    int center_x, int center_z, int near_radius, int far_radius, int quality) {
    size_t i, count = 0;
    if (!lod || !draws) return 0;
    for (i = 0; i < MAAGE_DISTANT_MAX_TILES && count < capacity; ++i) {
        const maage_distant_tile *tile = &lod->tiles[i];
        if (!tile->used || !tile->gpu ||
            maage_client_lod_quality_at(tile->x, tile->z,
                center_x, center_z, near_radius, far_radius, quality) < 0) continue;
        if (lod_distance(tile->x, tile->z, center_x, center_z) <= near_radius + 2) continue;
        draws[count++] = tile->gpu;
    }
    return count;
}

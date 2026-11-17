#include "distant_lod.h"
#include "lod.h"
#include "maage/storage.h"
#include "maage/geometry.h"
#include "maage/geometry_materials.h"
#include "maage/jobs.h"
#include "maage/fluids.h"
#include "maage/log.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <errno.h>

#define LOD_GENERATOR_VERSION 3UL
#define LOD_HEADER_WORDS 16U
#define LOD_SAMPLE_WORDS 7U
#define LOD_DISK_WORDS (LOD_HEADER_WORDS + 256U * LOD_SAMPLE_WORDS)
#define LOD_DISK_BYTES (LOD_DISK_WORDS * 4U)
#define LOD_NONE ((size_t) - 1)
#define LOD_DELETED ((size_t) - 2)

typedef struct lod_record {
    int x, z, span;
    unsigned long revision;
    maage_base_lod_column samples[256];
} lod_record;
typedef struct lod_appearance {
    maage_block_state_definition *states;
    maage_biome_definition *biomes;
    unsigned char *solid, *foliage;
    size_t state_count, biome_count, references;
} lod_appearance;
typedef struct lod_task {
    struct maage_distant_lod *owner;
    lod_appearance *appearance;
    maage_column_update *observed;
    maage_job_id job;
    unsigned long epoch, revision;
    int x, z, span, quality, force, hit, generated, cache_failed;
    maage_mesh_vertex *vertices;
    unsigned int *indices;
    maage_mesh_preparation *prepared;
    size_t gpu_bytes;
} lod_task;
typedef struct lod_tile {
    int used, x, z, span, quality, target, split, suppressed, failed, empty;
    size_t children[4];
    unsigned long selected, revision, gpu_revision;
    maage_render_mesh *gpu;
    lod_task *task;
    size_t gpu_bytes;
} lod_tile;
struct maage_distant_lod {
    char directory[MAAGE_PATH_CAPACITY];
    maage_base_lod_sampler *sampler; /* Single Jobs worker owns it exclusively. */
    const maage_block_registry *identity;
    lod_appearance *appearance;
    maage_world_seed seed;
    maage_generation_policy policy;
    maage_dimension_id dimension;
    uint32_t content, world;
    maage_jobs *jobs;
    unsigned job_capacity, active;
    int cache_warning;
    size_t cpu_budget, gpu_budget, gpu_bytes;
    lod_tile *tiles;
    size_t capacity, *hash, hash_capacity, *roots, root_count, eviction_cursor;
    unsigned long epoch, selection;
    int cx, cz, near_radius, far_radius, quality, planned, overflow;
    double view_x, view_z;
    maage_render_mesh *pending_gpu;
    lod_task *pending;
    size_t pending_index;
    maage_distant_lod_stats stats;
};

static void lod_write_face(maage_mesh_vertex *vertices, unsigned int *indices, size_t *faces,
                           const maage_block_state_definition *state, const maage_biome_definition *biome,
                           unsigned int face, float x0, float z0, float x1, float z1, float bottom, float top, int fluid) {
    static const unsigned int triangles[6] = {0U, 1U, 2U, 0U, 2U, 3U};
    const maage_block_face_definition *definition = &state->faces[face];
    const float *uv = state->fast_texture && state->leaf_fast_uv[2] > state->leaf_fast_uv[0] ? state->leaf_fast_uv : definition->uv;
    unsigned int k, component;
    float point[4][3], u, v, tint, shade;
    size_t offset = *faces * 4U;
    size_t elements = *faces * 6U;
    if (face == MAAGE_FACE_POSITIVE_Y) {
        point[0][0] = x0;
        point[0][1] = top;
        point[0][2] = z1;
        point[1][0] = x1;
        point[1][1] = top;
        point[1][2] = z1;
        point[2][0] = x1;
        point[2][1] = top;
        point[2][2] = z0;
        point[3][0] = x0;
        point[3][1] = top;
        point[3][2] = z0;
    } else if (face == MAAGE_FACE_NEGATIVE_X) {
        point[0][0] = x0;
        point[0][1] = bottom;
        point[0][2] = z0;
        point[1][0] = x0;
        point[1][1] = bottom;
        point[1][2] = z1;
        point[2][0] = x0;
        point[2][1] = top;
        point[2][2] = z1;
        point[3][0] = x0;
        point[3][1] = top;
        point[3][2] = z0;
    } else if (face == MAAGE_FACE_POSITIVE_X) {
        point[0][0] = x1;
        point[0][1] = bottom;
        point[0][2] = z1;
        point[1][0] = x1;
        point[1][1] = bottom;
        point[1][2] = z0;
        point[2][0] = x1;
        point[2][1] = top;
        point[2][2] = z0;
        point[3][0] = x1;
        point[3][1] = top;
        point[3][2] = z1;
    } else if (face == MAAGE_FACE_NEGATIVE_Z) {
        point[0][0] = x1;
        point[0][1] = bottom;
        point[0][2] = z0;
        point[1][0] = x0;
        point[1][1] = bottom;
        point[1][2] = z0;
        point[2][0] = x0;
        point[2][1] = top;
        point[2][2] = z0;
        point[3][0] = x1;
        point[3][1] = top;
        point[3][2] = z0;
    } else {
        point[0][0] = x0;
        point[0][1] = bottom;
        point[0][2] = z1;
        point[1][0] = x1;
        point[1][1] = bottom;
        point[1][2] = z1;
        point[2][0] = x1;
        point[2][1] = top;
        point[2][2] = z1;
        point[3][0] = x0;
        point[3][1] = top;
        point[3][2] = z1;
    }
    shade = face == MAAGE_FACE_POSITIVE_Y ? 1.0f : (face == MAAGE_FACE_NEGATIVE_X || face == MAAGE_FACE_POSITIVE_X ? 0.6f : 0.8f);
    for (k = 0; k < 4U; ++k) {
        maage_mesh_vertex *vertex = &vertices[offset + k];
        u = (k == 1U || k == 2U) ? 1.0f : 0.0f;
        v = k < 2U ? 1.0f : 0.0f;
        vertex->position[0] = point[k][0];
        vertex->position[1] = point[k][1];
        vertex->position[2] = point[k][2];
        vertex->uv[0] = u * (x1 - x0 > 4.0f ? 4.0f : x1 - x0);
        vertex->uv[1] = v * (z1 - z0 > 4.0f ? 4.0f : z1 - z0);
        memcpy(vertex->tile, uv, sizeof(vertex->tile));
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
        if (fluid && state->fluid_definition && state->fluid_definition->shader_water) vertex->rotation_face = MAAGE_MESH_EFFECT_WATER << MAAGE_MESH_EFFECT_SHIFT;
        else if (state->fast_texture) vertex->rotation_face = MAAGE_MESH_EFFECT_FOLIAGE << MAAGE_MESH_EFFECT_SHIFT;
    }
    for (k = 0; k < 6U; ++k)
        indices[elements + k] = (unsigned int) offset + triangles[k];
    ++*faces;
}

static maage_status lod_status(unsigned char code, unsigned char operation) {
    return maage_status_make(code, code == MAAGE_STATUS_SUCCESS || code == MAAGE_STATUS_NOOP ? 0 : MAAGE_CONTEXT_CLIENT, operation, 0);
}
static unsigned long lod_setting(const char *key, unsigned long fallback, unsigned long minimum, unsigned long maximum) {
    const char *value = getenv(key);
    char *end;
    unsigned long number;
    if (!value || !*value) return fallback;
    errno = 0;
    number = strtoul(value, &end, 10);
    return errno || *end || number < minimum || number > maximum ? fallback : number;
}
size_t maage_distant_lod_capacity(const maage_distant_lod *lod) {
    return lod ? lod->capacity : 128U;
}
static void lod_appearance_release(lod_appearance *appearance) {
    if (!appearance || --appearance->references) return;
    free(appearance->states);
    free(appearance->biomes);
    free(appearance->solid);
    free(appearance->foliage);
    free(appearance);
}
maage_status maage_distant_lod_set_appearance(maage_distant_lod *lod, const maage_block_registry *blocks, const maage_biome_registry *biomes) {
    lod_appearance *appearance;
    const maage_block_state_definition *state;
    const maage_block_definition *block;
    size_t i;
    if (!lod || !blocks || !biomes) return lod_status(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_OPERATION_UPDATE);
    appearance = (lod_appearance *) calloc(1, sizeof(*appearance));
    if (!appearance) return lod_status(MAAGE_STATUS_ALLOCATION_FAILED, MAAGE_OPERATION_CREATE);
    appearance->references = 1;
    appearance->state_count = maage_block_registry_state_count(blocks);
    appearance->biome_count = maage_biome_registry_count(biomes);
    appearance->states = (maage_block_state_definition *) calloc(appearance->state_count, sizeof(*appearance->states));
    appearance->biomes = (maage_biome_definition *) calloc(appearance->biome_count, sizeof(*appearance->biomes));
    appearance->solid = (unsigned char *) calloc(appearance->state_count, 1);
    appearance->foliage = (unsigned char *) calloc(appearance->state_count, 1);
    if (!appearance->states || !appearance->biomes || !appearance->solid || !appearance->foliage) {
        lod_appearance_release(appearance);
        return lod_status(MAAGE_STATUS_ALLOCATION_FAILED, MAAGE_OPERATION_CREATE);
    }
    for (i = 0; i < appearance->state_count; ++i) {
        state = maage_block_registry_state(blocks, (maage_block_state_id) i);
        if (!state) continue;
        appearance->states[i] = *state;
        block = maage_block_registry_block(blocks, state->block_id);
        appearance->solid[i] = block && block->solid && block->mesh_shape == MAAGE_BLOCK_MESH_CUBE;
        appearance->foliage[i] = block && block->foliage;
    }
    for (i = 0; i < appearance->biome_count; ++i) {
        const maage_biome_definition *biome = maage_biome_registry_get(biomes, (maage_biome_id) i);
        if (biome) appearance->biomes[i] = *biome;
    }
    lod_appearance_release(lod->appearance);
    lod->appearance = appearance;
    return lod_status(MAAGE_STATUS_SUCCESS, MAAGE_OPERATION_UPDATE);
}
static uint32_t lod_hash_bytes(uint32_t hash, const unsigned char *bytes, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i)
        hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    return hash;
}
static void lod_put(unsigned char *bytes, size_t word, uint32_t value) {
    unsigned int i;
    for (i = 0; i < 4; ++i)
        bytes[word * 4 + i] = (unsigned char) (value >> (i * 8));
}
static uint32_t lod_get(const unsigned char *bytes, size_t word) {
    return (uint32_t) bytes[word * 4] | (uint32_t) bytes[word * 4 + 1] << 8 | (uint32_t) bytes[word * 4 + 2] << 16 | (uint32_t) bytes[word * 4 + 3] << 24;
}
static int lod_path(const maage_distant_lod *lod, int x, int z, int span, int observed, int temporary, char path[MAAGE_PATH_CAPACITY]) {
    int length = snprintf(path, MAAGE_PATH_CAPACITY, "%s/%d_%d_%d.%s%s", lod->directory, x, z, span, observed ? "mgdo" : "mgdl", temporary ? ".tmp" : "");
    return length > 0 && length < MAAGE_PATH_CAPACITY;
}
static void lod_encode(const maage_distant_lod *lod, const lod_record *record, unsigned char bytes[LOD_DISK_BYTES]) {
    size_t i, offset;
    const maage_base_lod_column *sample;
    memset(bytes, 0, LOD_DISK_BYTES);
    memcpy(bytes, "MGLOD003", 8);
    lod_put(bytes, 2, LOD_GENERATOR_VERSION);
    lod_put(bytes, 3, (uint32_t) lod->seed.high);
    lod_put(bytes, 4, (uint32_t) lod->seed.low);
    lod_put(bytes, 5, (uint32_t) lod->dimension);
    lod_put(bytes, 6, lod->content);
    lod_put(bytes, 7, (uint32_t) lod->policy.sea_level);
    lod_put(bytes, 8, (uint32_t) lod->policy.spawn_search_radius);
    lod_put(bytes, 9, (uint32_t) lod->policy.spawn_search_step);
    lod_put(bytes, 10, (uint32_t) record->x);
    lod_put(bytes, 11, (uint32_t) record->z);
    lod_put(bytes, 12, (uint32_t) record->span);
    lod_put(bytes, 13, (uint32_t) record->revision);
    lod_put(bytes, 14, lod->world);
    for (i = 0; i < 256; ++i) {
        offset = LOD_HEADER_WORDS + i * LOD_SAMPLE_WORDS;
        sample = &record->samples[i];
        lod_put(bytes, offset, (uint32_t) sample->height);
        lod_put(bytes, offset + 1, (uint32_t) sample->surface);
        lod_put(bytes, offset + 2, (uint32_t) sample->biome);
        lod_put(bytes, offset + 3, (uint32_t) sample->water_height);
        lod_put(bytes, offset + 4, (uint32_t) sample->water);
        lod_put(bytes, offset + 5, (uint32_t) sample->canopy_height);
        lod_put(bytes, offset + 6, (uint32_t) sample->canopy);
    }
    lod_put(bytes, 15, lod_hash_bytes(UINT32_C(2166136261), bytes, LOD_DISK_BYTES));
}
static int lod_decode(const maage_distant_lod *lod, const lod_appearance *appearance, int x, int z, int span, unsigned char bytes[LOD_DISK_BYTES], lod_record *record) {
    uint32_t checksum = lod_get(bytes, 15);
    size_t i, offset;
    maage_base_lod_column *sample;
    lod_put(bytes, 15, 0);
    if (memcmp(bytes, "MGLOD003", 8) || lod_get(bytes, 2) != LOD_GENERATOR_VERSION ||
        lod_get(bytes, 3) != (uint32_t) lod->seed.high || lod_get(bytes, 4) != (uint32_t) lod->seed.low ||
        lod_get(bytes, 5) != (uint32_t) lod->dimension || lod_get(bytes, 6) != lod->content ||
        lod_get(bytes, 7) != (uint32_t) lod->policy.sea_level || lod_get(bytes, 8) != (uint32_t) lod->policy.spawn_search_radius ||
        lod_get(bytes, 9) != (uint32_t) lod->policy.spawn_search_step || lod_get(bytes, 10) != (uint32_t) x || lod_get(bytes, 11) != (uint32_t) z ||
        lod_get(bytes, 12) != (uint32_t) span || lod_get(bytes, 14) != lod->world ||
        checksum != lod_hash_bytes(UINT32_C(2166136261), bytes, LOD_DISK_BYTES)) return 0;
    memset(record, 0, sizeof(*record));
    record->x = x;
    record->z = z;
    record->span = span;
    record->revision = lod_get(bytes, 13);
    for (i = 0; i < 256; ++i) {
        offset = LOD_HEADER_WORDS + i * LOD_SAMPLE_WORDS;
        sample = &record->samples[i];
        sample->height = (int) lod_get(bytes, offset);
        sample->surface = lod_get(bytes, offset + 1);
        sample->biome = lod_get(bytes, offset + 2);
        sample->water_height = (int) lod_get(bytes, offset + 3);
        sample->water = lod_get(bytes, offset + 4);
        sample->canopy_height = (int) lod_get(bytes, offset + 5);
        sample->canopy = lod_get(bytes, offset + 6);
        if (sample->height < 0 || sample->height > MAAGE_WORLD_HEIGHT || sample->water_height < 0 || sample->water_height > MAAGE_WORLD_HEIGHT ||
            sample->canopy_height < 0 || sample->canopy_height > MAAGE_WORLD_HEIGHT || sample->surface >= appearance->state_count || sample->water >= appearance->state_count || sample->canopy >= appearance->state_count || sample->biome >= appearance->biome_count) return 0;
    }
    return 1;
}
static int lod_load(lod_task *task, int x, int z, int span, int observed, lod_record *record) {
    unsigned char bytes[LOD_DISK_BYTES];
    char path[MAAGE_PATH_CAPACITY];
    FILE *file;
    size_t count;
    int extra;
    if (!lod_path(task->owner, x, z, span, observed, 0, path)) return 0;
    file = maage_storage_open(path, "rb");
    if (!file) return 0;
    count = fread(bytes, 1, sizeof(bytes), file);
    extra = fgetc(file);
    fclose(file);
    return count == sizeof(bytes) && extra == EOF && lod_decode(task->owner, task->appearance, x, z, span, bytes, record);
}
static maage_status lod_save(lod_task *task, const lod_record *record, int observed) {
    unsigned char bytes[LOD_DISK_BYTES];
    char path[MAAGE_PATH_CAPACITY], temporary[MAAGE_PATH_CAPACITY];
    FILE *file;
    maage_status result;
    if (!lod_path(task->owner, record->x, record->z, record->span, observed, 0, path) || !lod_path(task->owner, record->x, record->z, record->span, observed, 1, temporary)) return lod_status(MAAGE_STATUS_BUFFER_OVERFLOW, MAAGE_OPERATION_SAVE);
    result = maage_storage_directories(task->owner->directory);
    if (maage_status_failed(result)) return result;
    lod_encode(task->owner, record, bytes);
    file = maage_storage_open(temporary, "wb");
    if (!file) return lod_status(MAAGE_STATUS_IO_ERROR, MAAGE_OPERATION_SAVE);
    result = fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes) ? maage_storage_sync(file) : lod_status(MAAGE_STATUS_IO_ERROR, MAAGE_OPERATION_WRITE);
    if (fclose(file) != 0 && !maage_status_failed(result)) result = lod_status(MAAGE_STATUS_IO_ERROR, MAAGE_OPERATION_SAVE);
    if (!maage_status_failed(result)) result = maage_storage_replace(temporary, path);
    if (maage_status_failed(result)) (void) maage_storage_remove(temporary);
    return result;
}
static maage_block_state_id lod_packet_state(const maage_column_update *packet, int x, int y, int z) {
    const maage_column_section *section = &packet->sections[y / 16];
    unsigned int cell = (unsigned int) ((y % 16) * 256 + z * 16 + x), index = 0, bits = section->bits;
    if (!section->palette_count) return (maage_block_state_id) -1;
    if (bits == 16) index = section->packed[cell * 2] | (unsigned int) section->packed[cell * 2 + 1] << 8;
    else if (bits) index = (section->packed[cell * bits / 8] >> (cell * bits % 8)) & ((1U << bits) - 1U);
    return index < section->palette_count ? section->palette[index] : (maage_block_state_id) -1;
}
static maage_status lod_summarize(lod_task *task, const maage_job_token *token) {
    lod_record record;
    maage_base_lod_column *sample;
    const maage_block_state_definition *state;
    maage_block_state_id id;
    int x, y, z, span;
    maage_status result;
    char path[MAAGE_PATH_CAPACITY];
    memset(&record, 0, sizeof(record));
    record.x = task->x;
    record.z = task->z;
    record.span = 1;
    record.revision = task->observed->revision;
    for (z = 0; z < 16; ++z) {
        if (maage_job_cancelled(token)) return lod_status(MAAGE_STATUS_INTERRUPTED, MAAGE_OPERATION_GENERATE);
        for (x = 0; x < 16; ++x) {
            sample = &record.samples[z * 16 + x];
            sample->biome = task->observed->biomes[z * 16 + x];
            for (y = MAAGE_WORLD_HEIGHT - 1; y >= 0; --y) {
                id = lod_packet_state(task->observed, x, y, z);
                if (id >= task->appearance->state_count) continue;
                state = &task->appearance->states[id];
                if (state->fluid_kind) {
                    if (!sample->water_height) {
                        sample->water_height = y + 1;
                        sample->water = id;
                    }
                } else if (task->appearance->foliage[id]) {
                    if (!sample->canopy_height) {
                        sample->canopy_height = y + 1;
                        sample->canopy = id;
                    }
                } else if (task->appearance->solid[id]) {
                    sample->height = y + 1;
                    sample->surface = id;
                    break;
                }
            }
            if (sample->water_height <= sample->height) sample->water_height = 0;
            if (sample->canopy_height <= sample->height) sample->canopy_height = 0;
        }
    }
    /* Only derived ancestors are invalidated. One worker serializes all writes. */
    for (span = 1; span <= 16; span *= 2) {
        int tx = task->x / span, tz = task->z / span;
        if (task->x % span < 0) --tx;
        if (task->z % span < 0) --tz;
        if (lod_path(task->owner, tx * span, tz * span, span, 0, 0, path) && maage_storage_status(path) == 1) {
            result = maage_storage_remove(path);
            if (maage_status_failed(result)) return result;
        }
    }
    /* Invalidate before atomic observation promotion: interruption can cause
     * a cache miss, never leave an old derived tile valid over a new summary. */
    return lod_save(task, &record, 1);
}
static int lod_sample(lod_task *task, int x, int z, lod_record *observed, int *cached_x, int *cached_z, maage_base_lod_column *sample) {
    int cx = maage_world_chunk_coordinate(x), cz = maage_world_chunk_coordinate(z);
    if (cx != *cached_x || cz != *cached_z) {
        *cached_x = cx;
        *cached_z = cz;
        if (!lod_load(task, cx, cz, 1, 1, observed)) observed->span = 0;
    }
    if (observed->span) {
        *sample = observed->samples[maage_world_local_coordinate(z) * 16 + maage_world_local_coordinate(x)];
        return 1;
    }
    return maage_base_lod_sampler_sample(task->owner->sampler, x, z, sample);
}
static int lod_top_equal(const maage_base_lod_column *a, const maage_base_lod_column *b, int kind) {
    if (a->biome != b->biome) return 0;
    if (kind == 1) return a->water_height > a->height && b->water_height > b->height && a->water_height == b->water_height && a->water == b->water;
    if (kind == 2) return a->canopy_height > a->height && b->canopy_height > b->height && a->canopy_height == b->canopy_height && a->canopy == b->canopy;
    return a->height > 0 && a->height == b->height && a->surface == b->surface;
}
static unsigned int lod_top_end(maage_base_lod_column samples[18][18], unsigned int x, unsigned int z, unsigned int patches, int kind, int merge) {
    unsigned int end = x;
    if (!merge) return x;
    if (x > 1 && lod_top_equal(&samples[z][x - 1], &samples[z][x], kind)) return 0;
    while (end < patches && lod_top_equal(&samples[z][end], &samples[z][end + 1], kind))
        ++end;
    return end;
}
static maage_status lod_build(lod_task *task, const maage_job_token *token) {
    lod_record record, observed;
    maage_base_lod_column border[18][18], *center, *neighbor;
    maage_mesh_data data;
    const maage_block_state_definition *state, *water, *canopy;
    const maage_biome_definition *biome;
    static const int dx[4] = {-1, 1, 0, 0}, dz[4] = {0, 0, -1, 1};
    static const unsigned int sides[4] = {MAAGE_FACE_NEGATIVE_X, MAAGE_FACE_POSITIVE_X, MAAGE_FACE_NEGATIVE_Z, MAAGE_FACE_POSITIVE_Z};
    unsigned int step = task->span == 1 ? (1U << task->quality) : 2U;
    unsigned int patches = 16U / step, x, z, side, end;
    int spacing = (int) step * task->span, sample_offset = task->span >= 8 ? task->span / 2 - 3 : task->span / 2, wx, wz, cx = INT_MAX, cz = INT_MAX, floor;
    float x0, z0, x1, z1;
    size_t faces = 0, capacity = (size_t) patches * patches * 15U;
    maage_status result;
    memset(&record, 0, sizeof(record));
    if (!task->force && lod_load(task, task->x, task->z, task->span, 0, &record)) task->hit = 1;
    else {
        record.x = task->x;
        record.z = task->z;
        record.span = task->span;
        for (z = 0; z < 16; ++z) {
            if (maage_job_cancelled(token)) return lod_status(MAAGE_STATUS_INTERRUPTED, MAAGE_OPERATION_GENERATE);
            for (x = 0; x < 16; ++x) {
                wx = task->x * 16 + (int) x * task->span + sample_offset;
                wz = task->z * 16 + (int) z * task->span + sample_offset;
                if (!lod_sample(task, wx, wz, &observed, &cx, &cz, &record.samples[z * 16 + x])) return lod_status(MAAGE_STATUS_INVALID_STATE, MAAGE_OPERATION_GENERATE);
            }
        }
        task->generated = 1;
        result = lod_save(task, &record, 0);
        if (maage_status_failed(result)) {
            task->cache_failed = 1;
            MAAGE_STATUS_TRACE(maage_job_trace(token), result, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_SAVE);
        }
    }
    memset(border, 0, sizeof(border));
    for (z = 0; z < patches + 2; ++z) {
        if (maage_job_cancelled(token)) return lod_status(MAAGE_STATUS_INTERRUPTED, MAAGE_OPERATION_GENERATE);
        for (x = 0; x < patches + 2; ++x) {
            if (x && z && x <= patches && z <= patches) border[z][x] = record.samples[(z - 1) * step * 16 + (x - 1) * step];
            else {
                wx = task->x * 16 + ((int) x - 1) * spacing + sample_offset;
                wz = task->z * 16 + ((int) z - 1) * spacing + sample_offset;
                (void) lod_sample(task, wx, wz, &observed, &cx, &cz, &border[z][x]);
            }
        }
    }
    task->vertices = (maage_mesh_vertex *) calloc(capacity * 4U, sizeof(*task->vertices));
    task->indices = (unsigned int *) malloc(capacity * 6U * sizeof(*task->indices));
    if (!task->vertices || !task->indices) return lod_status(MAAGE_STATUS_ALLOCATION_FAILED, MAAGE_OPERATION_CREATE);
    for (z = 1; z <= patches; ++z)
        for (x = 1; x <= patches; ++x) {
            center = &border[z][x];
            if (center->surface >= task->appearance->state_count || center->biome >= task->appearance->biome_count) continue;
            state = &task->appearance->states[center->surface];
            water = center->water < task->appearance->state_count ? &task->appearance->states[center->water] : NULL;
            canopy = center->canopy < task->appearance->state_count ? &task->appearance->states[center->canopy] : NULL;
            biome = &task->appearance->biomes[center->biome];
            x0 = (float) (task->x * 16 + ((int) x - 1) * spacing);
            z0 = (float) (task->z * 16 + ((int) z - 1) * spacing);
            x1 = x0 + spacing;
            z1 = z0 + spacing;
            end = lod_top_end(border, x, z, patches, 0, task->span > 1 || task->quality > 0);
            if (center->height > 0 && end) lod_write_face(task->vertices, task->indices, &faces, state, biome, MAAGE_FACE_POSITIVE_Y, x0, z0, x0 + (end - x + 1) * spacing, z1, 0, (float) center->height, 0);
            for (side = 0; side < 4; ++side) {
                neighbor = &border[(int) z + dz[side]][(int) x + dx[side]];
                floor = neighbor->height;
                /* Opaque skirts bridge adjacent resolution steps. Water uses shared
             * planes and shoreline walls; translucent skirts would z-fight. */
                if ((side == 0 && x == 1) || (side == 1 && x == patches) || (side == 2 && z == 1) || (side == 3 && z == patches)) floor = 0;
                if (floor < center->height) lod_write_face(task->vertices, task->indices, &faces, state, biome, sides[side], x0, z0, x1, z1, (float) floor, (float) center->height, 0);
                if (canopy && center->canopy_height > center->height) {
                    floor = neighbor->canopy_height > center->height ? neighbor->canopy_height : center->height;
                    if (floor < center->canopy_height) lod_write_face(task->vertices, task->indices, &faces, canopy, biome, sides[side], x0, z0, x1, z1, (float) floor, (float) center->canopy_height, 0);
                }
            }
            end = lod_top_end(border, x, z, patches, 2, task->span > 1 || task->quality > 0);
            if (canopy && center->canopy_height > center->height && end) lod_write_face(task->vertices, task->indices, &faces, canopy, biome, MAAGE_FACE_POSITIVE_Y, x0, z0, x0 + (end - x + 1) * spacing, z1, 0, (float) center->canopy_height, 0);
            if (water && center->water_height > center->height) {
                end = lod_top_end(border, x, z, patches, 1, task->span > 1 || task->quality > 0);
                if (end) lod_write_face(task->vertices, task->indices, &faces, water, biome, MAAGE_FACE_POSITIVE_Y, x0, z0, x0 + (end - x + 1) * spacing, z1, 0, (float) center->water_height - 0.10f, 1);
                for (side = 0; side < 4; ++side) {
                    neighbor = &border[(int) z + dz[side]][(int) x + dx[side]];
                    floor = neighbor->height > neighbor->water_height ? neighbor->height : neighbor->water_height;
                    if (floor < center->height) floor = center->height;
                    if (floor < center->water_height) lod_write_face(task->vertices, task->indices, &faces, water, biome, sides[side], x0, z0, x1, z1, (float) floor, (float) center->water_height - 0.10f, 1);
                }
            }
        }
    if (!faces) return lod_status(MAAGE_STATUS_NOOP, MAAGE_OPERATION_CREATE);
    memset(&data, 0, sizeof(data));
    data.vertices = task->vertices;
    data.vertex_count = faces * 4U;
    data.indices = task->indices;
    data.index_count = faces * 6U;
    /* Conservative packed vertices + worst-case surface records + native and
     * two frame-slot sorted 32-bit index views. Admission includes replacement
     * and old geometry concurrently, until deferred retirement. */
    task->gpu_bytes = data.vertex_count * 64U + data.index_count * 12U;
    result = maage_mesh_prepare(&data, &task->prepared);
    if (!maage_status_failed(result) && task->cache_failed) return lod_status(MAAGE_STATUS_PARTIAL, MAAGE_OPERATION_SAVE);
    return result;
}
static void lod_run(void *context, const maage_job_token *token) {
    lod_task *task = (lod_task *) context;
    maage_status result = task->observed ? lod_summarize(task, token) : lod_build(task, token);
    if (maage_status_failed(result)) MAAGE_STATUS_TRACE(maage_job_trace(token), result, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_GENERATE);
    (void) maage_job_set_result(token, result);
}
static void lod_task_free(void *context) {
    lod_task *task = (lod_task *) context;
    if (!task) return;
    maage_column_update_destroy(task->observed);
    maage_mesh_preparation_destroy(task->prepared);
    lod_appearance_release(task->appearance);
    free(task->vertices);
    free(task->indices);
    free(task);
}
static size_t lod_key(int x, int z, int span) {
    uint32_t hash = (uint32_t) x * UINT32_C(0x9e3779b1) ^ (uint32_t) z * UINT32_C(0x85ebca77) ^ (uint32_t) span * UINT32_C(0xc2b2ae3d);
    hash ^= hash >> 16;
    return (size_t) hash;
}
static void lod_hash_rebuild(maage_distant_lod *lod) {
    size_t i, bucket;
    memset(lod->hash, 0xff, lod->hash_capacity * sizeof(*lod->hash));
    for (i = 0; i < lod->capacity; ++i)
        if (lod->tiles[i].used) {
            bucket = lod_key(lod->tiles[i].x, lod->tiles[i].z, lod->tiles[i].span) % lod->hash_capacity;
            while (lod->hash[bucket] != LOD_NONE)
                bucket = (bucket + 1) % lod->hash_capacity;
            lod->hash[bucket] = i;
        }
}
static size_t lod_find(const maage_distant_lod *lod, int x, int z, int span) {
    size_t bucket = lod_key(x, z, span) % lod->hash_capacity, index;
    while ((index = lod->hash[bucket]) != LOD_NONE) {
        const lod_tile *tile;
        if (index == LOD_DELETED) {
            bucket = (bucket + 1) % lod->hash_capacity;
            continue;
        }
        tile = &lod->tiles[index];
        if (tile->x == x && tile->z == z && tile->span == span) return index;
        bucket = (bucket + 1) % lod->hash_capacity;
    }
    return LOD_NONE;
}
static int lod_max(int a, int b) {
    return a > b ? a : b;
}
static int lod_distance(const maage_distant_lod *lod, int x, int z, int span) {
    int dx = lod->cx < x ? x - lod->cx : (lod->cx >= x + span ? lod->cx - (x + span - 1) : 0);
    int dz = lod->cz < z ? z - lod->cz : (lod->cz >= z + span ? lod->cz - (z + span - 1) : 0);
    return lod_max(dx, dz);
}
static size_t lod_node(maage_distant_lod *lod, int x, int z, int span) {
    size_t index, i, bucket, oldest = LOD_NONE;
    lod_tile *tile;
    int distance = lod_distance(lod, x, z, span), half, split, target, clipped;
    if (distance > lod->far_radius || x * 16 >= MAAGE_WORLD_LIMIT || z * 16 >= MAAGE_WORLD_LIMIT || (x + span) * 16 <= -MAAGE_WORLD_LIMIT || (z + span) * 16 <= -MAAGE_WORLD_LIMIT) return LOD_NONE;
    clipped = x * 16 < -MAAGE_WORLD_LIMIT || z * 16 < -MAAGE_WORLD_LIMIT || (x + span) * 16 > MAAGE_WORLD_LIMIT || (z + span) * 16 > MAAGE_WORLD_LIMIT;
    if (lod_max(abs(x - lod->cx), abs(x + span - 1 - lod->cx)) <= lod->near_radius + 2 && lod_max(abs(z - lod->cz), abs(z + span - 1 - lod->cz)) <= lod->near_radius + 2) return LOD_NONE;
    if (span == 1 && (distance <= lod->near_radius + 2 || maage_client_lod_quality_at(x, z, lod->cx, lod->cz, lod->near_radius, lod->far_radius, lod->quality) < 0)) return LOD_NONE;
    split = span > 1 && (clipped || distance < lod->near_radius + 6 + span * (lod->quality == 0 ? 4 : 2));
    target = span == 1 ? (distance <= lod->near_radius + 4 ? 0 : maage_client_lod_quality_at(x, z, lod->cx, lod->cz, lod->near_radius, lod->far_radius, lod->quality)) : 1;
    index = lod_find(lod, x, z, span);
    if (index != LOD_NONE && span > 1 && !clipped && distance > lod->near_radius + 6) {
        int threshold = lod->near_radius + 6 + span * (lod->quality == 0 ? 4 : 2);
        split = lod->tiles[index].split ? distance < threshold + 2 : distance < threshold - 2;
    }
    if (index == LOD_NONE) {
        for (i = 0; i < lod->capacity; ++i) {
            size_t candidate = lod->eviction_cursor++ % lod->capacity;
            tile = &lod->tiles[candidate];
            if (!tile->used || (tile->selected != lod->selection && !tile->task && (!lod->pending || candidate != lod->pending_index))) {
                oldest = candidate;
                break;
            }
        }
        if (oldest == LOD_NONE) {
            lod->overflow = 1;
            return LOD_NONE;
        }
        index = oldest;
        tile = &lod->tiles[index];
        if (tile->used) {
            bucket = lod_key(tile->x, tile->z, tile->span) % lod->hash_capacity;
            while (lod->hash[bucket] != index)
                bucket = (bucket + 1) % lod->hash_capacity;
            lod->hash[bucket] = LOD_DELETED;
            lod->gpu_bytes -= tile->gpu_bytes;
            maage_render_mesh_destroy(tile->gpu);
            memset(tile, 0, sizeof(*tile));
        }
        tile->used = 1;
        tile->x = x;
        tile->z = z;
        tile->span = span;
        tile->quality = -1;
        tile->failed = 0;
        tile->empty = 0;
        bucket = lod_key(x, z, span) % lod->hash_capacity;
        while (lod->hash[bucket] != LOD_NONE && lod->hash[bucket] != LOD_DELETED)
            bucket = (bucket + 1) % lod->hash_capacity;
        lod->hash[bucket] = index;
    }
    tile = &lod->tiles[index];
    if (tile->failed == 2) tile->failed = 0;
    tile->selected = lod->selection;
    tile->target = target;
    tile->split = split;
    for (i = 0; i < 4; ++i)
        tile->children[i] = LOD_NONE;
    if (split) {
        half = span / 2;
        for (i = 0; i < 4; ++i)
            tile->children[i] = lod_node(lod, x + (int) (i % 2) * half, z + (int) (i / 2) * half, half);
    }
    return index;
}
static size_t lod_node_count(const maage_distant_lod *lod, int x, int z, int span) {
    int distance = lod_distance(lod, x, z, span), half;
    size_t count = 1, i;
    if (distance > lod->far_radius || x * 16 >= MAAGE_WORLD_LIMIT || z * 16 >= MAAGE_WORLD_LIMIT || (x + span) * 16 <= -MAAGE_WORLD_LIMIT || (z + span) * 16 <= -MAAGE_WORLD_LIMIT) return 0;
    if (lod_max(abs(x - lod->cx), abs(x + span - 1 - lod->cx)) <= lod->near_radius + 2 && lod_max(abs(z - lod->cz), abs(z + span - 1 - lod->cz)) <= lod->near_radius + 2) return 0;
    if (span == 1) return distance > lod->near_radius + 2 && maage_client_lod_quality_at(x, z, lod->cx, lod->cz, lod->near_radius, lod->far_radius, lod->quality) >= 0 ? 1 : 0;
    if (x * 16 < -MAAGE_WORLD_LIMIT || z * 16 < -MAAGE_WORLD_LIMIT || (x + span) * 16 > MAAGE_WORLD_LIMIT || (z + span) * 16 > MAAGE_WORLD_LIMIT || distance < lod->near_radius + 8 + span * (lod->quality == 0 ? 4 : 2)) {
        half = span / 2;
        for (i = 0; i < 4; ++i)
            count += lod_node_count(lod, x + (int) (i % 2) * half, z + (int) (i / 2) * half, half);
    }
    return count;
}
static int lod_reserve(maage_distant_lod *lod, size_t needed) {
    size_t limit = lod->cpu_budget / (sizeof(lod_tile) + sizeof(size_t) * 3U), capacity, hash_capacity;
    lod_tile *tiles;
    size_t *hash, *roots;
    if (needed <= lod->capacity) return 1;
    capacity = needed + needed / 2U;
    if (capacity > limit) capacity = limit;
    if (capacity <= lod->capacity) return 0;
    hash_capacity = capacity * 2U + 1U;
    hash = (size_t *) malloc(hash_capacity * sizeof(*hash));
    roots = (size_t *) malloc(capacity * sizeof(*roots));
    if (!hash || !roots) {
        free(hash);
        free(roots);
        return 0;
    }
    tiles = (lod_tile *) realloc(lod->tiles, capacity * sizeof(*tiles));
    if (!tiles) {
        free(hash);
        free(roots);
        return 0;
    }
    memset(tiles + lod->capacity, 0, (capacity - lod->capacity) * sizeof(*tiles));
    free(lod->hash);
    free(lod->roots);
    lod->tiles = tiles;
    lod->capacity = capacity;
    lod->hash_capacity = hash_capacity;
    lod->hash = hash;
    lod->roots = roots;
    return capacity >= needed;
}
static void lod_plan(maage_distant_lod *lod, int cx, int cz, int near_radius, int far_radius, int quality) {
    int x, z, first_x, first_z, end_x, end_z;
    size_t root, needed = 0;
    lod->cx = cx;
    lod->cz = cz;
    lod->near_radius = near_radius;
    lod->far_radius = far_radius;
    lod->quality = quality;
    lod->planned = 1;
    lod->root_count = 0;
    lod->overflow = 0;
    ++lod->selection;
    first_x = (cx - far_radius) / 16;
    if ((cx - far_radius) % 16 < 0) --first_x;
    first_z = (cz - far_radius) / 16;
    if ((cz - far_radius) % 16 < 0) --first_z;
    end_x = (cx + far_radius) / 16;
    if ((cx + far_radius) % 16 < 0) --end_x;
    end_z = (cz + far_radius) / 16;
    if ((cz + far_radius) % 16 < 0) --end_z;
    if (far_radius > near_radius + 2)
        for (z = first_z; z <= end_z; ++z)
            for (x = first_x; x <= end_x; ++x)
                needed += lod_node_count(lod, x * 16, z * 16, 16);
    if (!lod_reserve(lod, needed)) lod->overflow = 1;
    lod_hash_rebuild(lod);
    if (far_radius > near_radius + 2)
        for (z = first_z; z <= end_z; ++z)
            for (x = first_x; x <= end_x; ++x) {
                root = lod_node(lod, x * 16, z * 16, 16);
                if (root != LOD_NONE && lod->root_count < lod->capacity) lod->roots[lod->root_count++] = root;
            }
}
static void lod_cancel_upload(maage_distant_lod *lod) {
    maage_render_mesh_destroy(lod->pending_gpu);
    lod->pending_gpu = NULL;
    lod_task_free(lod->pending);
    lod->pending = NULL;
}
static void lod_invalidate_column(maage_distant_lod *lod, int x, int z) {
    size_t i;
    for (i = 0; i < lod->capacity; ++i) {
        lod_tile *tile = &lod->tiles[i];
        if (!tile->used || x < tile->x || z < tile->z || x >= tile->x + tile->span || z >= tile->z + tile->span) continue;
        ++tile->revision;
        tile->failed = 0;
        tile->empty = 0;
        if (tile->task) (void) maage_jobs_cancel(lod->jobs, tile->task->job);
        if (lod->pending && lod->pending_index == i) lod_cancel_upload(lod);
    }
}
maage_status maage_distant_lod_observe(maage_distant_lod *lod, const maage_world *world, int x, int z) {
    lod_task *task;
    maage_status result;
    if (!lod || lod->active >= lod->job_capacity) return lod_status(MAAGE_STATUS_NOOP, MAAGE_OPERATION_SNAPSHOT);
    task = (lod_task *) calloc(1, sizeof(*task));
    if (!task) return lod_status(MAAGE_STATUS_ALLOCATION_FAILED, MAAGE_OPERATION_SNAPSHOT);
    result = maage_world_copy_column(world, x, z, &task->observed);
    if (maage_status_code(result) != MAAGE_STATUS_SUCCESS) {
        free(task);
        return result;
    }
    task->owner = lod;
    task->appearance = lod->appearance;
    ++task->appearance->references;
    task->x = x;
    task->z = z;
    task->span = 1;
    task->epoch = lod->epoch;
    result = maage_jobs_submit(lod->jobs, 0, lod_run, task, &task->job, NULL);
    if (maage_status_failed(result)) {
        lod_task_free(task);
        return result;
    }
    ++lod->active;
    lod_invalidate_column(lod, x, z);
    return result;
}
void maage_distant_lod_invalidate_gpu(maage_distant_lod *lod) {
    size_t i;
    if (!lod) return;
    ++lod->epoch;
    lod_cancel_upload(lod);
    if (lod->tiles)
        for (i = 0; i < lod->capacity; ++i) {
            lod_tile *tile = &lod->tiles[i];
            if (tile->task) (void) maage_jobs_cancel(lod->jobs, tile->task->job);
            maage_render_mesh_destroy(tile->gpu);
            tile->gpu = NULL;
            tile->quality = -1;
            tile->failed = 0;
            tile->empty = 0;
            tile->gpu_bytes = 0;
        }
    lod->planned = 0;
    lod->gpu_bytes = 0;
    lod->stats.resident = lod->stats.publication_backlog = 0;
    lod->stats.gpu_bytes_estimate = 0;
    memset(lod->stats.by_level, 0, sizeof(lod->stats.by_level));
}
maage_distant_lod *maage_distant_lod_create(const char *instance_directory, const char world_id[37], maage_dimension_id dimension, maage_world_seed seed, const maage_generation_policy *policy, const maage_block_registry *blocks, const maage_biome_registry *biomes) {
    maage_distant_lod *lod;
    char relative[128];
    const char *name;
    size_t i;
    maage_status result;
    if (!instance_directory || !world_id || !maage_storage_uuid_valid(world_id) || !dimension || !blocks || !biomes || !policy) return NULL;
    lod = (maage_distant_lod *) calloc(1, sizeof(*lod));
    if (!lod) return NULL;
    lod->seed = seed;
    lod->policy = *policy;
    lod->dimension = dimension;
    lod->identity = maage_block_registry_identity_source(blocks);
    lod->cpu_budget = (size_t) lod_setting("MAAGE_LOD_CPU_MIB", 16, 1, 256) * 1048576U;
    lod->gpu_budget = (size_t) lod_setting("MAAGE_LOD_GPU_MIB", 512, 32, (size_t) - 1 / 1048576U < 4096U ? (unsigned long) ((size_t) - 1 / 1048576U) : 4096UL) * 1048576U;
    lod->capacity = maage_distant_lod_capacity(NULL);
    lod->hash_capacity = lod->capacity * 2 + 1;
    lod->tiles = (lod_tile *) calloc(lod->capacity, sizeof(*lod->tiles));
    lod->hash = (size_t *) malloc(lod->hash_capacity * sizeof(*lod->hash));
    lod->roots = (size_t *) malloc(lod->capacity * sizeof(*lod->roots));
    lod->sampler = maage_base_lod_sampler_create(lod->identity, biomes->view_source ? biomes->view_source : biomes, seed, policy);
    if (!lod->tiles || !lod->hash || !lod->roots || !lod->sampler) goto failed;
    lod_hash_rebuild(lod);
    result = maage_distant_lod_set_appearance(lod, blocks, biomes);
    if (maage_status_failed(result)) goto failed;
    lod->world = lod_hash_bytes(UINT32_C(2166136261), (const unsigned char *) world_id, 36);
    lod->content = UINT32_C(2166136261);
    for (i = 0; i < lod->appearance->state_count; ++i) {
        unsigned char identity[16];
        unsigned int face;
        size_t assignment;
        const maage_traits_registry *traits = maage_block_registry_traits(blocks);
        const maage_property_set *properties;
        const maage_block_state_definition *state = &lod->appearance->states[i];
        lod_put(identity, 0, (uint32_t) state->properties);
        lod_put(identity, 1, (uint32_t) state->fluid_kind);
        lod_put(identity, 2, (uint32_t) state->fluid_level);
        lod_put(identity, 3, (uint32_t) state->geometry_variant);
        lod->content = lod_hash_bytes(lod->content, identity, sizeof(identity));
        properties = maage_property_set_get(traits, state->properties);
        for (assignment = 0; properties && assignment < properties->count; ++assignment) {
            const maage_property_assignment *value = &properties->values[assignment];
            name = maage_property_name(traits, value->property);
            if (name) lod->content = lod_hash_bytes(lod->content, (const unsigned char *) name, strlen(name) + 1);
            lod_put(identity, 0, (uint32_t) value->value);
            lod->content = lod_hash_bytes(lod->content, identity, 4);
            name = maage_property_value_name(traits, value->property, value->value);
            if (name) lod->content = lod_hash_bytes(lod->content, (const unsigned char *) name, strlen(name) + 1);
        }
        for (face = 0; face < MAAGE_FACE_COUNT; ++face) {
            lod_put(identity, 0, (uint32_t) state->faces[face].texture_id);
            lod_put(identity, 1, (uint32_t) state->faces[face].overlay_texture);
            lod_put(identity, 2, (uint32_t) state->faces[face].tint_mode);
            lod_put(identity, 3, (uint32_t) state->faces[face].texture_rotation);
            lod->content = lod_hash_bytes(lod->content, identity, sizeof(identity));
        }
        name = maage_block_registry_block_name(blocks, state->block_id);
        if (name) lod->content = lod_hash_bytes(lod->content, (const unsigned char *) name, strlen(name) + 1);
    }
    for (i = 0; i < lod->appearance->biome_count; ++i) {
        const maage_biome_definition *biome = maage_biome_registry_get(biomes, (maage_biome_id) i);
        if (biome) {
            lod->content = lod_hash_bytes(lod->content, (const unsigned char *) &biome->base_height, sizeof(biome->base_height));
            lod->content = lod_hash_bytes(lod->content, (const unsigned char *) &biome->variation, sizeof(biome->variation));
            lod->content = lod_hash_bytes(lod->content, (const unsigned char *) &biome->temperature, sizeof(biome->temperature));
            lod->content = lod_hash_bytes(lod->content, (const unsigned char *) &biome->rainfall, sizeof(biome->rainfall));
            if (biome->generation_data) lod->content = lod_hash_bytes(lod->content, (const unsigned char *) biome->generation_data, biome->generation_data_size);
        }
        name = maage_biome_registry_name(biomes, (maage_biome_id) i);
        if (name) lod->content = lod_hash_bytes(lod->content, (const unsigned char *) name, strlen(name) + 1);
    }
    snprintf(relative, sizeof(relative), "worlds/%s/lod-cache/v3/d%lu", world_id, (unsigned long) dimension);
    if (maage_status_failed(maage_storage_join(lod->directory, sizeof(lod->directory), instance_directory, relative))) goto failed;
    lod->job_capacity = (unsigned) lod_setting("MAAGE_LOD_JOBS", (unsigned long) (lod->cpu_budget / (4U * 1048576U) > 2 ? lod->cpu_budget / (4U * 1048576U) : 2), 2, UINT_MAX);
    result = maage_jobs_create(1, lod->job_capacity, &lod->jobs, NULL);
    if (maage_status_failed(result)) goto failed;
    lod->epoch = 1;
    return lod;
failed:
    maage_distant_lod_destroy(lod);
    return NULL;
}
maage_status maage_distant_lod_destroy(maage_distant_lod *lod) {
    maage_status result;
    maage_job_completion completion;
    if (!lod) return lod_status(MAAGE_STATUS_NOOP, MAAGE_OPERATION_DESTROY);
    maage_distant_lod_invalidate_gpu(lod);
    if (lod->jobs) {
        /* Lifecycle only. GPU handles have already been released by their owner. */
        while (lod->active) {
            result = maage_jobs_poll(lod->jobs, &completion);
            if (maage_status_code(result) == MAAGE_STATUS_SUCCESS) {
                lod_task *task = (lod_task *) completion.context;
                size_t index = lod_find(lod, task->x, task->z, task->span);
                if (index != LOD_NONE && lod->tiles[index].task == task) lod->tiles[index].task = NULL;
                --lod->active;
                lod_task_free(completion.context);
            } else if (maage_status_failed(maage_jobs_wait(lod->jobs))) break;
        }
        result = maage_jobs_destroy(lod->jobs, lod_task_free);
        if (maage_status_failed(result)) {
            maage_log_status(MAAGE_LOG_ERROR, "maage.client.lod", result, "Distant worker join failed; borrowed resources retained.");
            return result;
        }
    }
    maage_base_lod_sampler_destroy(lod->sampler);
    lod_appearance_release(lod->appearance);
    free(lod->roots);
    free(lod->hash);
    free(lod->tiles);
    free(lod);
    return lod_status(MAAGE_STATUS_SUCCESS, MAAGE_OPERATION_DESTROY);
}
int maage_distant_lod_ready(const maage_distant_lod *lod, int x, int z, int quality) {
    size_t index;
    const lod_tile *tile;
    (void) quality;
    if (!lod) return 0;
    index = lod_find(lod, x, z, 1);
    if (index == LOD_NONE) return 0;
    tile = &lod->tiles[index];
    return tile->selected == lod->selection && (tile->gpu || tile->empty) && tile->gpu_revision == tile->revision && tile->quality <= tile->target;
}
void maage_distant_lod_suppress(maage_distant_lod *lod, int x, int z) {
    size_t index;
    if (!lod) return;
    index = lod_find(lod, x, z, 1);
    if (index != LOD_NONE) lod->tiles[index].suppressed = 1;
}
static maage_status lod_poll(maage_distant_lod *lod, maage_renderer *renderer, maage_render_texture *atlas) {
    maage_job_completion completion;
    maage_status result;
    lod_task *task;
    size_t index;
    lod_tile *tile;
    if (lod->pending) {
        tile = &lod->tiles[lod->pending_index];
        if (lod->pending->epoch != lod->epoch || lod->pending->revision != tile->revision || tile->selected != lod->selection) {
            lod_cancel_upload(lod);
            ++lod->stats.stale;
        } else {
            result = maage_render_mesh_upload_step(lod->pending_gpu, 262144U, NULL);
            if (maage_status_code(result) == MAAGE_STATUS_NOOP) return result;
            if (maage_status_failed(result)) {
                tile->failed = 1;
                lod_cancel_upload(lod);
                return result;
            }
            lod->gpu_bytes -= tile->gpu_bytes;
            tile->gpu_bytes = lod->pending->gpu_bytes;
            lod->gpu_bytes += tile->gpu_bytes;
            maage_render_mesh_destroy(tile->gpu);
            tile->gpu = lod->pending_gpu;
            lod->pending_gpu = NULL;
            tile->empty = 0;
            tile->quality = lod->pending->quality;
            tile->gpu_revision = tile->revision;
            ++lod->stats.published;
            lod_task_free(lod->pending);
            lod->pending = NULL;
        }
    }
    result = maage_jobs_poll(lod->jobs, &completion);
    if (maage_status_code(result) != MAAGE_STATUS_SUCCESS) return result;
    --lod->active;
    task = (lod_task *) completion.context;
    lod->stats.generation_ms = completion.run_seconds * 1000;
    lod->stats.cache_hits += task->hit;
    lod->stats.generated += task->generated;
    if (task->cache_failed && !lod->cache_warning) {
        lod->cache_warning = 1;
        maage_log_trace_status(MAAGE_LOG_WARNING, "maage.client.lod", completion.operation_result, &completion.trace, "Distant tile generated but its optional cache write failed; repeated cache failures suppressed for this world.");
    }
    if (task->observed) {
        if (!completion.cancelled && !maage_status_failed(completion.result)) lod_invalidate_column(lod, task->x, task->z);
        if (maage_status_failed(completion.operation_result) && !lod->cache_warning) {
            lod->cache_warning = 1;
            maage_log_trace_status(MAAGE_LOG_WARNING, "maage.client.lod", completion.operation_result, &completion.trace, "Observed distant surface cache write failed; repeated cache failures suppressed for this world.");
        }
        lod_task_free(task);
        return lod_status(MAAGE_STATUS_SUCCESS, MAAGE_OPERATION_UPDATE);
    }
    index = lod_find(lod, task->x, task->z, task->span);
    tile = index == LOD_NONE ? NULL : &lod->tiles[index];
    if (tile && tile->task == task) tile->task = NULL;
    if (completion.cancelled || task->epoch != lod->epoch || !tile || tile->selected != lod->selection || task->revision != tile->revision || task->quality != tile->target) {
        ++lod->stats.stale;
        lod_task_free(task);
        return lod_status(MAAGE_STATUS_NOOP, MAAGE_OPERATION_UPDATE);
    }
    if (maage_status_failed(completion.result) || !task->prepared) {
        result = completion.result;
        if (maage_status_failed(result)) tile->failed = 1;
        else {
            maage_render_mesh_destroy(tile->gpu);
            tile->gpu = NULL;
            lod->gpu_bytes -= tile->gpu_bytes;
            tile->gpu_bytes = 0;
            tile->empty = 1;
            tile->gpu_revision = tile->revision;
            tile->quality = task->quality;
        }
        lod_task_free(task);
        return result;
    }
    if (task->gpu_bytes > lod->gpu_budget - lod->gpu_bytes) {
        size_t i;
        for (i = 0; i < lod->capacity && task->gpu_bytes > lod->gpu_budget - lod->gpu_bytes; ++i) {
            lod_tile *retired = &lod->tiles[i];
            if (!retired->gpu || retired->selected == lod->selection) continue;
            maage_render_mesh_destroy(retired->gpu);
            retired->gpu = NULL;
            lod->gpu_bytes -= retired->gpu_bytes;
            retired->gpu_bytes = 0;
        }
        if (task->gpu_bytes > lod->gpu_budget - lod->gpu_bytes) {
            lod->overflow = 1;
            tile->failed = 2;
            lod_task_free(task);
            return lod_status(MAAGE_STATUS_PARTIAL, MAAGE_OPERATION_UPLOAD);
        }
    }
    result = maage_renderer_begin_geometry(renderer, &task->prepared, atlas, &lod->pending_gpu, &completion.trace);
    if (maage_status_failed(result)) {
        tile->failed = 1;
        lod_task_free(task);
        return result;
    }
    lod->pending = task;
    lod->pending_index = index;
    return lod_status(MAAGE_STATUS_NOOP, MAAGE_OPERATION_UPLOAD);
}
maage_status maage_distant_lod_step(maage_distant_lod *lod, maage_renderer *renderer, maage_render_texture *atlas, int center_x, int center_z, int near_radius, int far_radius, int quality, double yaw) {
    maage_status result;
    size_t i, best;
    unsigned priority, best_priority;
    lod_task *task;
    lod_tile *tile;
    if (!lod || !renderer || !atlas || quality < 0 || quality > 2 || near_radius < 1 || far_radius < 1 || far_radius > 128) return lod_status(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_OPERATION_UPDATE);
    lod->view_x = sin(yaw);
    lod->view_z = -cos(yaw);
    if (!lod->planned || lod->cx != center_x || lod->cz != center_z || lod->near_radius != near_radius || lod->far_radius != far_radius || lod->quality != quality) {
        lod_plan(lod, center_x, center_z, near_radius, far_radius, quality);
        for (i = 0; i < lod->capacity; ++i)
            if (lod->tiles[i].task && lod->tiles[i].selected != lod->selection) (void) maage_jobs_cancel(lod->jobs, lod->tiles[i].task->job);
    }
    for (i = 0; i < lod->capacity; ++i)
        lod->tiles[i].suppressed = 0;
    result = lod_poll(lod, renderer, atlas);
    if (maage_status_failed(result)) return result;
    if (!lod->pending && lod->active + 1 < lod->job_capacity) {
        best = LOD_NONE;
        best_priority = UINT_MAX;
        for (i = 0; i < lod->capacity; ++i) {
            tile = &lod->tiles[i];
            if (!tile->used || tile->selected != lod->selection || tile->task || tile->failed || ((tile->gpu || tile->empty) && tile->quality == tile->target && tile->gpu_revision == tile->revision)) continue;
            if (tile->split && lod_distance(lod, tile->x, tile->z, tile->span) <= lod->near_radius + 6) continue;
            priority = (unsigned) lod_distance(lod, tile->x, tile->z, tile->span) * 16U + (unsigned) (16 / tile->span);
            if ((tile->x + tile->span * 0.5 - center_x) * lod->view_x + (tile->z + tile->span * 0.5 - center_z) * lod->view_z < 0) priority += 64U;
            if (tile->gpu) priority += 4096U;
            if (priority < best_priority) {
                best_priority = priority;
                best = i;
            }
        }
        if (best != LOD_NONE) {
            tile = &lod->tiles[best];
            task = (lod_task *) calloc(1, sizeof(*task));
            if (!task) return lod_status(MAAGE_STATUS_ALLOCATION_FAILED, MAAGE_OPERATION_CREATE);
            task->owner = lod;
            task->appearance = lod->appearance;
            ++task->appearance->references;
            task->x = tile->x;
            task->z = tile->z;
            task->span = tile->span;
            task->quality = tile->target;
            task->epoch = lod->epoch;
            task->revision = tile->revision;
            task->force = tile->revision != 0;
            result = maage_jobs_submit(lod->jobs, best_priority + 1U, lod_run, task, &task->job, NULL);
            if (maage_status_failed(result)) {
                lod_task_free(task);
                return result;
            }
            tile->task = task;
            ++lod->active;
        }
    }
    lod->stats.active = lod->active;
    lod->stats.publication_backlog = lod->pending != NULL;
    lod->stats.resident = 0;
    lod->stats.gpu_bytes_estimate = 0;
    memset(lod->stats.by_level, 0, sizeof(lod->stats.by_level));
    for (i = 0; i < lod->capacity; ++i)
        if (lod->tiles[i].gpu) {
            int level = 0, span = lod->tiles[i].span;
            while (span > 1) {
                ++level;
                span /= 2;
            }
            ++lod->stats.by_level[level];
            ++lod->stats.resident;
            lod->stats.gpu_bytes_estimate += lod->tiles[i].gpu_bytes;
        }
    lod->stats.budget_limited = lod->overflow;
    return lod_status(lod->overflow ? MAAGE_STATUS_PARTIAL : MAAGE_STATUS_SUCCESS, MAAGE_OPERATION_UPDATE);
}
static int lod_covered(const maage_distant_lod *lod, size_t index) {
    const lod_tile *tile;
    size_t i;
    if (index == LOD_NONE) return 1;
    tile = &lod->tiles[index];
    if (tile->gpu || tile->empty) return 1;
    if (!tile->split) return 0;
    for (i = 0; i < 4; ++i)
        if (!lod_covered(lod, tile->children[i])) return 0;
    return 1;
}
static int lod_visible(const lod_tile *tile, const float *matrix) {
    int plane, corner, outside;
    double p[4], x, y, z;
    if (!matrix) return 1;
    for (plane = 0; plane < 6; ++plane) {
        outside = 1;
        for (corner = 0; corner < 8; ++corner) {
            x = tile->x * 16.0 + (corner & 1 ? tile->span * 16.0 : 0);
            y = corner & 2 ? MAAGE_WORLD_HEIGHT : 0;
            z = tile->z * 16.0 + (corner & 4 ? tile->span * 16.0 : 0);
            p[0] = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
            p[1] = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
            p[2] = matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14];
            p[3] = matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
            if ((plane == 0 && p[0] >= -p[3]) || (plane == 1 && p[0] <= p[3]) || (plane == 2 && p[1] >= -p[3]) || (plane == 3 && p[1] <= p[3]) || (plane == 4 && p[2] >= 0) || (plane == 5 && p[2] <= p[3])) {
                outside = 0;
                break;
            }
        }
        if (outside) return 0;
    }
    return 1;
}
static size_t lod_append_node(const maage_distant_lod *lod, size_t index, const maage_render_mesh **draws, size_t capacity, const float *matrix) {
    const lod_tile *tile;
    size_t i, count = 0;
    int children_ready = 1;
    if (index == LOD_NONE || !capacity) return 0;
    tile = &lod->tiles[index];
    if (!lod_visible(tile, matrix)) return 0;
    if (tile->split)
        for (i = 0; i < 4; ++i)
            if (!lod_covered(lod, tile->children[i])) children_ready = 0;
    if (tile->split && (children_ready || !tile->gpu || lod_distance(lod, tile->x, tile->z, tile->span) <= lod->near_radius + 6)) {
        for (i = 0; i < 4; ++i)
            count += lod_append_node(lod, tile->children[i], draws + count, capacity - count, matrix);
    } else if (tile->gpu && !tile->suppressed) draws[count++] = tile->gpu;
    return count;
}
size_t maage_distant_lod_append(const maage_distant_lod *lod, const maage_render_mesh **draws, size_t capacity, const float matrix[16]) {
    size_t i, count = 0;
    if (!lod || !draws) return 0;
    for (i = 0; i < lod->root_count; ++i)
        count += lod_append_node(lod, lod->roots[i], draws + count, capacity - count, matrix);
    return count;
}
void maage_distant_lod_get_stats(const maage_distant_lod *lod, maage_distant_lod_stats *stats) {
    if (stats) {
        if (lod) *stats = lod->stats;
        else memset(stats, 0, sizeof(*stats));
    }
}

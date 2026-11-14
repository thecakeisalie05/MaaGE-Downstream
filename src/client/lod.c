#include "lod.h"
#include "prediction.h"
#include "maage/geometry.h"
#include "maage/blocks.h"
#include "maage/biome.h"
#include "maage/geometry_materials.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int lod_distance(int a, int b) {
    return a < b ? b : a;
}
/* LOD is deliberately block-aligned. The precipitation heightmap includes
 * leaves and structures; using it directly as elevation produces giant smooth
 * triangular canopies. Scan for the biome's registered ground state instead.
 * All observations are read-only from the already-resident replica. */
typedef struct maage_lod_surface {
    int ground, water, present;
    const maage_block_state_definition *ground_state, *water_state;
    const maage_biome_definition *biome;
} maage_lod_surface;

static maage_lod_surface lod_sample(const maage_world *world, const maage_block_registry *registry,
    const maage_biome_registry *biomes, int x, int z) {
    maage_lod_surface surface;
    maage_world_observation observation;
    const maage_block_state_definition *state, *fallback_state = NULL;
    const maage_block_definition *block;
    int y, top, fallback_height = 0;
    memset(&surface, 0, sizeof(surface));
    if (!maage_world_observe(world, x, 0, z, &observation)) return surface;
    surface.present = 1;
    surface.biome = biomes ? maage_biome_registry_get(biomes, observation.biome_id) : NULL;
    top = observation.precipitation_height;
    if (top > MAAGE_WORLD_HEIGHT) top = MAAGE_WORLD_HEIGHT;
    /* A fluid surface may sit above terrain, including ocean floor. */
    for (y = top - 1; y >= 0; --y) {
        state = maage_block_registry_state(registry, maage_world_get(world, x, y, z));
        if (!state) continue;
        block = maage_block_registry_block(registry, state->block_id);
        if (!block) continue;
        if (state->fluid_kind) {
            if (!surface.water_state) {
                surface.water_state = state;
                surface.water = y + 1;
            }
            continue;
        }
        if (!block->solid || block->foliage || block->mesh_shape != MAAGE_BLOCK_MESH_CUBE) continue;
        if (!fallback_state) {
            fallback_state = state;
            fallback_height = y + 1;
        }
        if (surface.biome && (state->id == surface.biome->surface || state->id == surface.biome->filler)) {
            surface.ground_state = state;
            surface.ground = y + 1;
            break;
        }
    }
    if (!surface.ground_state) {
        surface.ground_state = fallback_state;
        surface.ground = fallback_height;
    }
    if (surface.water <= surface.ground) {
        surface.water_state = NULL;
        surface.water = 0;
    }
    return surface;
}

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

/* One flat top, exposed step faces, and a separate water plane per patch.
 * Geometric capacity is bounded at six quads/patch. No sloped triangles or
 * canopy heights; the original near meshes stay authoritative for appearance. */
static maage_status lod_build(maage_world *world, const maage_biome_registry *biomes, int cx, int cz, int quality,
    maage_mesh_vertex **vertices, unsigned int **indices, maage_mesh_data *data) {
    const maage_block_registry *registry = maage_world_registry(world);
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
    if (!world || !registry || !vertices || !indices || !data || quality < 0 || quality > 2)
        return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_CREATE, 0);
    *vertices = NULL;
    *indices = NULL;
    memset(data, 0, sizeof(*data));
    step = quality == 0 ? 1U : (quality == 1 ? 2U : 4U);
    patches = 16U / step;
    capacity = (size_t) patches * patches * 6U;
    v = (maage_mesh_vertex *) calloc(capacity * 4U, sizeof(*v));
    ix = (unsigned int *) malloc(capacity * 6U * sizeof(*ix));
    if (!v || !ix) {
        free(v);
        free(ix);
        return maage_status_make(MAAGE_STATUS_ALLOCATION_FAILED, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_CREATE, 0);
    }
    for (z = 0; z < patches + 2U; ++z) for (x = 0; x < patches + 2U; ++x) {
        wx = cx * 16 + ((int) x - 1) * (int) step + (int) step / 2;
        wz = cz * 16 + ((int) z - 1) * (int) step + (int) step / 2;
        samples[z][x] = lod_sample(world, registry, biomes, wx, wz);
    }
    for (z = 1U; z <= patches; ++z) for (x = 1U; x <= patches; ++x) {
        center = &samples[z][x];
        if (!center->ground_state) continue;
        x0 = (float) (cx * 16 + ((int) x - 1) * (int) step);
        z0 = (float) (cz * 16 + ((int) z - 1) * (int) step);
        x1 = x0 + (float) step;
        z1 = z0 + (float) step;
        lod_write_face(v, ix, &count, center->ground_state, center->biome, MAAGE_FACE_POSITIVE_Y,
            x0, z0, x1, z1, 0.0f, (float) center->ground, 0);
        for (side = 0U; side < 4U; ++side) {
            neighbor = &samples[(int) z + dz[side]][(int) x + dx[side]];
            /* Missing neighbors are unobserved, not an infinitely deep void. */
            neighbor_heights[side] = neighbor->present ? neighbor->ground : center->ground;
            if (neighbor_heights[side] < center->ground)
                lod_write_face(v, ix, &count, center->ground_state, center->biome, faces[side],
                    x0, z0, x1, z1, (float) neighbor_heights[side], (float) center->ground, 0);
        }
        if (center->water_state && center->water > center->ground)
            lod_write_face(v, ix, &count, center->water_state, center->biome, MAAGE_FACE_POSITIVE_Y,
                x0, z0, x1, z1, 0.0f, (float) center->water - 0.10f, 1);
    }
    if (!count) {
        free(v);
        free(ix);
        return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_CREATE, 0);
    }
    data->vertices = v;
    data->vertex_count = count * 4U;
    data->indices = ix;
    data->index_count = count * 6U;
    *vertices = v;
    *indices = ix;
    return maage_status_make(MAAGE_STATUS_SUCCESS, 0, MAAGE_OPERATION_CREATE, 0);
}
void maage_client_lod_mesh_clear(maage_client_lod_mesh *mesh) {
    if (!mesh) return;
    maage_render_mesh_destroy(mesh->gpu);
    memset(mesh, 0, sizeof(*mesh));
}
void maage_client_lod_cancel(maage_client_lod *lod) {
    if (!lod) return;
    maage_render_mesh_destroy(lod->pending.gpu);
    free(lod->pending.vertices);
    free(lod->pending.indices);
    memset(&lod->pending, 0, sizeof(lod->pending));
}
void maage_client_lod_clear(maage_client_lod *lod) {
    maage_client_lod_cancel(lod);
    if (lod) lod->cursor = lod->completed = 0;
}
maage_status maage_client_lod_step(maage_client_lod *lod, maage_world *world,
    maage_renderer *renderer, maage_render_texture *atlas, const maage_biome_registry *biomes,
    maage_client_chunk_mesh *slots, size_t count,
    int center_x, int center_z, int detail_radius, int far_radius, int quality) {
    maage_client_lod_pending *pending;
    maage_mesh_data data;
    maage_mesh_preparation *prepared = NULL;
    maage_status result;
    size_t i, index;
    maage_client_chunk_mesh *source;
    int distance;
    if (!lod || !world || !renderer || !atlas || (!slots && count) || quality < 0 || quality > 2)
        return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_UPDATE, 0);
    pending = &lod->pending;
    if (pending->gpu) {
        if (pending->slot >= count || !slots[pending->slot].used ||
            slots[pending->slot].visual_revision != pending->revision ||
            (distance = lod_distance(abs(slots[pending->slot].x - center_x), abs(slots[pending->slot].z - center_z))) <= detail_radius ||
            distance > far_radius || pending->quality != quality) {
            maage_client_lod_cancel(lod);
            return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_UPDATE, 0);
        }
        result = maage_render_mesh_upload_step(pending->gpu, 262144U, NULL);
        if (maage_status_code(result) == MAAGE_STATUS_NOOP) return result;
        if (maage_status_failed(result)) {
            maage_client_lod_cancel(lod);
            return result;
        }
        maage_client_lod_mesh_clear(&slots[pending->slot].lod);
        slots[pending->slot].lod.gpu = pending->gpu;
        slots[pending->slot].lod.quality = pending->quality;
        slots[pending->slot].lod.revision = pending->revision;
        pending->gpu = NULL;
        free(pending->vertices);
        free(pending->indices);
        memset(pending, 0, sizeof(*pending));
        ++lod->completed;
        return result;
    }
    if (!count || far_radius <= detail_radius) return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_UPDATE, 0);
    for (i = 0; i < count; ++i) {
        index = (lod->cursor + i) % count;
        source = &slots[index];
        if (!source->used || !source->mesh ||
            !maage_world_chunk_loaded(world, source->x, source->z)) continue;
        distance = lod_distance(abs(source->x - center_x), abs(source->z - center_z));
        if (distance <= detail_radius || distance > far_radius) continue;
        if (source->lod.gpu && source->lod.revision == source->visual_revision && source->lod.quality == quality) continue;
        result = lod_build(world, biomes, source->x, source->z, quality, &pending->vertices, &pending->indices, &data);
        if (maage_status_failed(result)) return result;
        if (maage_status_code(result) == MAAGE_STATUS_NOOP) {
            /* Empty resident columns have no surface to decimate. Never
             * submit an empty mesh to the GPU geometry preparation API. */
            lod->cursor = (index + 1) % count;
            return result;
        }
        result = maage_mesh_prepare(&data, &prepared);
        if (!maage_status_failed(result)) result = maage_renderer_begin_geometry(renderer, &prepared, atlas, &pending->gpu, NULL);
        maage_mesh_preparation_destroy(prepared);
        if (maage_status_failed(result)) {
            maage_client_lod_cancel(lod);
            return result;
        }
        pending->slot = index;
        pending->revision = source->visual_revision;
        pending->quality = quality;
        lod->cursor = (index + 1) % count;
        return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_UPDATE, 0);
    }
    lod->cursor = (lod->cursor + 1) % count;
    return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_UPDATE, 0);
}

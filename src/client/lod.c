#include "lod.h"
#include "maage/geometry.h"
#include "maage/blocks.h"
#include "maage/geometry_materials.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int lod_distance(int a, int b) {
    return a < b ? b : a;
}
static int lod_height(maage_world *world, int x, int z, int fallback) {
    maage_world_observation sample;
    if (!maage_world_observe(world, x, 0, z, &sample)) return fallback;
    return sample.precipitation_height;
}
/* One top quad per 1x1, 2x2 or 4x4 terrain patch. Adjacent surface
 * heights are sampled from the same integer coordinates within each column.
 * This retains a bounded coarse silhouette, not precise decorations/caves. */
static maage_status lod_build(maage_world *world, int cx, int cz, int quality,
    maage_mesh_vertex **vertices, unsigned int **indices, maage_mesh_data *data) {
    const maage_block_registry *registry = maage_world_registry(world);
    const maage_block_state_definition *state;
    const maage_block_face_definition *face;
    maage_mesh_vertex *v;
    unsigned int *ix;
    unsigned int step = quality == 0 ? 1U : quality == 1 ? 2U : 4U;
    unsigned int count = 16U / step, i, j, k, base, ix_base;
    int wx, wz, h[4];
    float x[4], z[4], uv[4], tint[4];
    size_t quads = (size_t) count * count;
    if (!vertices || !indices || !data || !world || !registry) return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_CREATE, 0);
    *vertices = NULL;
    *indices = NULL;
    memset(data, 0, sizeof(*data));
    v = (maage_mesh_vertex *) calloc(quads * 4U, sizeof(*v));
    ix = (unsigned int *) malloc(quads * 6U * sizeof(*ix));
    if (!v || !ix) {
        free(v);
        free(ix);
        return maage_status_make(MAAGE_STATUS_ALLOCATION_FAILED, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_CREATE, 0);
    }
    for (j = 0; j < count; ++j) for (i = 0; i < count; ++i) {
        wx = cx * 16 + (int) (i * step);
        wz = cz * 16 + (int) (j * step);
        h[0] = lod_height(world, wx, wz + (int) step, 64);
        h[1] = lod_height(world, wx + (int) step, wz + (int) step, h[0]);
        h[2] = lod_height(world, wx + (int) step, wz, h[1]);
        h[3] = lod_height(world, wx, wz, h[0]);
        state = maage_block_registry_state(registry, maage_world_get(world, wx, h[3] > 0 ? h[3] - 1 : 0, wz));
        face = state ? &state->faces[MAAGE_FACE_POSITIVE_Y] : NULL;
        for (k = 0; k < 4; ++k) {
            uv[k] = face ? face->uv[k] : 0;
            tint[k] = face ? face->tint[k] : 1;
        }
        x[0] = x[3] = (float) wx;
        x[1] = x[2] = (float) wx + step;
        z[0] = z[1] = (float) wz + step;
        z[2] = z[3] = (float) wz;
        base = (j * count + i) * 4U;
        ix_base = (j * count + i) * 6U;
        for (k = 0; k < 4; ++k) {
            v[base + k].position[0] = x[k];
            v[base + k].position[1] = (float) h[k];
            v[base + k].position[2] = z[k];
            v[base + k].uv[0] = k == 1U || k == 2U ? uv[2] : uv[0];
            v[base + k].uv[1] = k <= 1U ? uv[3] : uv[1];
            v[base + k].color[0] = tint[0];
            v[base + k].color[1] = tint[1];
            v[base + k].color[2] = tint[2];
            v[base + k].color[3] = 1.0f;
            v[base + k].light[0] = 1.0f;
            v[base + k].light[1] = 0;
            v[base + k].material = MAAGE_MESH_MATERIAL_OPAQUE;
            v[base + k].tile[0] = uv[0];
            v[base + k].tile[1] = uv[1];
            v[base + k].tile[2] = uv[2];
            v[base + k].tile[3] = uv[3];
        }
        ix[ix_base + 0] = base + 0U;
        ix[ix_base + 1] = base + 1U;
        ix[ix_base + 2] = base + 2U;
        ix[ix_base + 3] = base + 0U;
        ix[ix_base + 4] = base + 2U;
        ix[ix_base + 5] = base + 3U;
    }
    data->vertices = v;
    data->vertex_count = quads * 4U;
    data->indices = ix;
    data->index_count = quads * 6U;
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
    maage_renderer *renderer, maage_render_texture *atlas,
    const maage_client_lod_source *sources, size_t count,
    int center_x, int center_z, int detail_radius, int far_radius, int quality) {
    maage_client_lod_pending *pending;
    maage_mesh_data data;
    maage_mesh_preparation *prepared = NULL;
    maage_status result;
    size_t i, index;
    const maage_client_lod_source *source;
    int distance;
    if (!lod || !world || !renderer || !atlas || (!sources && count) || quality < 0 || quality > 2)
        return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_UPDATE, 0);
    pending = &lod->pending;
    if (pending->gpu) {
        if (pending->slot >= count || !sources[pending->slot].present ||
            sources[pending->slot].revision != pending->revision ||
            sources[pending->slot].mesh == NULL ||
            (distance = lod_distance(abs(sources[pending->slot].x - center_x), abs(sources[pending->slot].z - center_z))) <= detail_radius ||
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
        maage_client_lod_mesh_clear(sources[pending->slot].mesh);
        sources[pending->slot].mesh->gpu = pending->gpu;
        sources[pending->slot].mesh->quality = pending->quality;
        sources[pending->slot].mesh->revision = pending->revision;
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
        source = &sources[index];
        if (!source->present || !source->mesh ||
            !maage_world_chunk_loaded(world, source->x, source->z)) continue;
        distance = lod_distance(abs(source->x - center_x), abs(source->z - center_z));
        if (distance <= detail_radius || distance > far_radius) continue;
        if (source->mesh->gpu && source->mesh->revision == source->revision && source->mesh->quality == quality) continue;
        result = lod_build(world, source->x, source->z, quality, &pending->vertices, &pending->indices, &data);
        if (maage_status_failed(result)) return result;
        result = maage_mesh_prepare(&data, &prepared);
        if (!maage_status_failed(result)) result = maage_renderer_begin_geometry(renderer, &prepared, atlas, &pending->gpu, NULL);
        maage_mesh_preparation_destroy(prepared);
        if (maage_status_failed(result)) {
            maage_client_lod_cancel(lod);
            return result;
        }
        pending->slot = index;
        pending->revision = source->revision;
        pending->quality = quality;
        lod->cursor = (index + 1) % count;
        return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_UPDATE, 0);
    }
    lod->cursor = (lod->cursor + 1) % count;
    return maage_status_make(MAAGE_STATUS_NOOP, 0, MAAGE_OPERATION_UPDATE, 0);
}

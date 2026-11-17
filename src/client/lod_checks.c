/* Offline regression checks use the actual tile planner/cache/job code and
 * native content registries. No renderer/window, accounts or World assets. */
#include "distant_lod.c"
#include "options.c"
#include "../core/render/render_effects_internal.h"
#include "../application/content_modules.h"
#include "../core/render/render_internal.h"
#include "maage/dimensions.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static void check_plan(maage_distant_lod *lod, int cx, int cz, int near_radius, int far_radius, int quality) {
    unsigned char coverage[257 * 257];
    size_t i, j;
    int x, z, dx, dz, distance;
    memset(coverage, 0, sizeof(coverage));
    lod_plan(lod, cx, cz, near_radius, far_radius, quality);
    assert(!lod->overflow);
    for (i = 0; i < lod->capacity; ++i) {
        const lod_tile *tile = &lod->tiles[i];
        if (!tile->used || tile->selected != lod->selection || tile->split) continue;
        assert(lod_find(lod, tile->x, tile->z, tile->span) == i);
        if (tile->span == 1 && lod_distance(lod, tile->x, tile->z, 1) <= near_radius + 4) assert(tile->target == 0);
        for (z = tile->z; z < tile->z + tile->span; ++z)
            for (x = tile->x; x < tile->x + tile->span; ++x) {
                dx = x - cx;
                dz = z - cz;
                if (abs(dx) > far_radius || abs(dz) > far_radius) continue;
                j = (size_t) (dz + 128) * 257 + dx + 128;
                assert(++coverage[j] == 1);
            }
    }
    for (dz = -far_radius; dz <= far_radius; ++dz)
        for (dx = -far_radius; dx <= far_radius; ++dx) {
            distance = lod_max(abs(dx), abs(dz));
            j = (size_t) (dz + 128) * 257 + dx + 128;
            if (cx + dx < -MAAGE_WORLD_LIMIT / 16 || cx + dx >= MAAGE_WORLD_LIMIT / 16 || cz + dz < -MAAGE_WORLD_LIMIT / 16 || cz + dz >= MAAGE_WORLD_LIMIT / 16 || distance <= near_radius + 2 || maage_client_lod_quality_at(cx + dx, cz + dz, cx, cz, near_radius, far_radius, quality) < 0) assert(!coverage[j]);
            else assert(coverage[j] == 1);
        }
}
static void check_cache(maage_distant_lod *lod) {
    lod_record record, decoded;
    unsigned char bytes[LOD_DISK_BYTES];
    size_t i;
    memset(&record, 0, sizeof(record));
    record.x = -17;
    record.z = -32;
    record.span = 4;
    record.revision = 17;
    for (i = 0; i < 256; ++i) {
        record.samples[i].height = 64;
        record.samples[i].surface = 1;
    }
    lod_encode(lod, &record, bytes);
    assert(lod_decode(lod, lod->appearance, -17, -32, 4, bytes, &decoded));
    assert(decoded.revision == 17 && decoded.samples[255].height == 64);
    lod_encode(lod, &record, bytes);
    bytes[80] ^= 1;
    assert(!lod_decode(lod, lod->appearance, -17, -32, 4, bytes, &decoded));
    lod_encode(lod, &record, bytes);
    assert(!lod_decode(lod, lod->appearance, -17, -32, 2, bytes, &decoded));
    lod_encode(lod, &record, bytes);
    ++lod->seed.low;
    assert(!lod_decode(lod, lod->appearance, -17, -32, 4, bytes, &decoded));
    --lod->seed.low;
    lod_encode(lod, &record, bytes);
    ++lod->dimension;
    assert(!lod_decode(lod, lod->appearance, -17, -32, 4, bytes, &decoded));
    --lod->dimension;
    lod_encode(lod, &record, bytes);
    ++lod->content;
    assert(!lod_decode(lod, lod->appearance, -17, -32, 4, bytes, &decoded));
    --lod->content;
    record.samples[0].height = MAAGE_WORLD_HEIGHT + 1;
    lod_encode(lod, &record, bytes);
    assert(!lod_decode(lod, lod->appearance, -17, -32, 4, bytes, &decoded));
}
static void check_frame_plan(void) {
    maage_renderer renderer;
    maage_render_frame_plan plan;
    maage_render_uniforms draw;
    maage_render_mesh_cursor cursor;
    maage_render_mesh mesh;
    float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memset(&renderer, 0, sizeof(renderer));
    memset(&mesh, 0, sizeof(mesh));
    memset(&cursor, 0, sizeof(cursor));
    renderer.capabilities.features = MAAGE_RENDER_CAP_SCENE;
    assert(maage_status_failed(maage_renderer_set_native_shaders(&renderer, 1, 1, 1)));
    renderer.capabilities.features |= MAAGE_RENDER_CAP_NATIVE_SHADERS;
    assert(!maage_status_failed(maage_renderer_set_native_shaders(&renderer, 0, 1, 1)));
    renderer.fog_distance[0] = 100;
    renderer.fog_distance[1] = 200;
    assert(!maage_status_failed(maage_render_frame_plan_prepare(&renderer, NULL, 0, matrix, NULL, 0, &plan)));
    assert(!plan.shaders_enabled && plan.world.fog_distance[0] == 100);
    assert(!maage_status_failed(maage_renderer_set_native_shaders(&renderer, 1, 2, 1.25f)));
    assert(!maage_status_failed(maage_render_frame_plan_prepare(&renderer, NULL, 0, matrix, NULL, 0, &plan)));
    assert(plan.shaders_enabled && plan.shader_exposure == 1.25f && plan.world.fog_distance[0] == 80 && plan.hand.fog_distance[1] == 0);
    maage_render_draw_uniforms(&plan, 1, &mesh, &cursor, &draw);
    assert(draw.fog_distance[1] == 0);
    renderer.ambient_light = 0.2f;
    assert(!maage_status_failed(maage_render_frame_plan_prepare(&renderer, NULL, 0, matrix, NULL, 0, &plan)));
    assert(plan.world.fog_distance[0] == 100);
    assert(!maage_status_failed(maage_render_frame_plan_prepare(&renderer, NULL, 0, NULL, NULL, 0, &plan)));
    assert(!plan.shaders_enabled);
    free(renderer.ordered);
}
static void check_effect_material_pack(void) {
    maage_mesh_vertex vertices[4];
    maage_mesh_data data;
    maage_mesh_preparation *prepared = NULL;
    maage_render_gpu_mesh_vertex packed[4], partial[4];
    unsigned int indices[6] = {0, 1, 2, 0, 2, 3};
    unsigned int i, channel;
    memset(vertices, 0, sizeof(vertices));
    memset(&data, 0, sizeof(data));
    for (i = 0; i < 4; ++i) {
        vertices[i].position[0] = i == 1 || i == 2 ? 1.0f : 0;
        vertices[i].position[1] = i >= 2 ? 1.0f : 0;
        vertices[i].material = MAAGE_MESH_MATERIAL_CUTOUT;
        vertices[i].rotation_face = MAAGE_MESH_EFFECT_ROOTED << MAAGE_MESH_EFFECT_SHIFT;
        for (channel = 0; channel < 4; ++channel)
            vertices[i].color[channel] = 1;
    }
    data.vertices = vertices;
    data.vertex_count = 4;
    data.indices = indices;
    data.index_count = 6;
    assert(!maage_status_failed(maage_mesh_prepare(&data, &prepared)));
    assert(prepared->geometry.bounds[0] == 0 && prepared->geometry.bounds[3] == 1 && prepared->geometry.bounds[4] == 1);
    maage_render_mesh_pack_span(prepared, 0, sizeof(packed), packed);
    maage_render_mesh_pack_span(prepared, 0, 37, partial);
    maage_render_mesh_pack_span(prepared, 37, sizeof(partial) - 37, (unsigned char *) partial + 37);
    assert(!memcmp(packed, partial, sizeof(packed)) && maage_render_gpu_layout_supported());
    assert(packed[0].padding[0] == 0 && packed[1].padding[0] == 0 && packed[2].padding[0] == 255 && packed[3].padding[0] == 255);
    assert(((unsigned int) prepared->surfaces[0].material_rotation / MAAGE_MESH_MATERIAL_COUNT >> MAAGE_MESH_EFFECT_SHIFT) == MAAGE_MESH_EFFECT_ROOTED);
    maage_mesh_preparation_destroy(prepared);
}
static void check_shader_effects(const char *directory) {
    maage_shader_settings settings, saved;
    maage_client_options options, loaded;
    maage_renderer renderer;
    maage_shader_frame frame;
    maage_render_frame_plan plan;
    maage_effect_uniforms uniforms, night;
    maage_instance *instance = NULL;
    char error[256], path[MAAGE_PATH_CAPACITY], line[96];
    int preset, section = 0;
    unsigned int i, j, k, control;
    float sum, center[4], bounds[6] = {-1, -1, -1, 1, 1, 1};
    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (preset = 0; preset < 3; ++preset) {
        assert(!maage_status_failed(maage_shader_settings_preset(&settings, preset)));
        assert(maage_shader_settings_valid(&settings) && maage_shader_settings_classify(&settings) == preset);
        saved = settings;
        assert(maage_status_failed(maage_shader_setting_set(&settings, MAAGE_SHADER_shadow_distance, 257)));
        assert(settings.shadow_distance == saved.shadow_distance);
        for (control = 0; control < MAAGE_SHADER_CONTROL_COUNT; ++control)
            assert(!maage_status_failed(maage_shader_setting_set(&settings, control, maage_shader_setting_get(&settings, control))));
        settings.wind = 93;
        assert(maage_shader_settings_classify(&settings) == 3);
    }
    maage_client_options_defaults(&options);
    assert(!options.shaders_enabled && options.shader_quality == 1 && options.shader_effects.shadow_distance == 96);
    strcpy(line, "[shaders]");
    maage_client_options_parse(&options, line, &section);
    strcpy(line, "shadow_distance=256");
    maage_client_options_parse(&options, line, &section);
    strcpy(line, "shadow_quality=3");
    maage_client_options_parse(&options, line, &section);
    strcpy(line, "wind=101");
    maage_client_options_parse(&options, line, &section);
    assert(options.shader_effects.shadow_distance == 256 && options.shader_effects.shadow_quality == 3 && options.shader_effects.wind == 40);
    if (directory) {
        assert(!maage_status_failed(maage_instance_create(directory, "Offline shader settings", "", "", &instance, error, NULL)));
        assert(!maage_status_failed(maage_instance_resolve_path(instance, MAAGE_INSTANCE_FOLDER_CONFIG, NULL, path, sizeof(path))));
        assert(!maage_status_failed(maage_storage_directories(path)));
        options.shader_quality = 3;
        options.shader_exposure = 125;
        assert(!maage_status_failed(maage_client_options_save(instance, &options, "")));
        assert(!maage_status_failed(maage_client_options_load(instance, &loaded, NULL)));
        assert(maage_client_options_equal(&options, &loaded));
        assert(!maage_status_failed(maage_instance_close(instance)));
    }
    memset(&renderer, 0, sizeof(renderer));
    memset(&frame, 0, sizeof(frame));
    memset(&plan, 0, sizeof(plan));
    maage_shader_settings_preset(&plan.effects, 1);
    plan.effect_frame.sun[0] = 0.6f;
    plan.effect_frame.sun[1] = 0.8f;
    plan.effect_frame.sun[3] = 0.8f;
    plan.effect_frame.moon[0] = -0.6f;
    plan.effect_frame.moon[1] = -0.8f;
    plan.effect_frame.weather[2] = 1;
    plan.world.matrix[0] = plan.world.matrix[5] = 1;
    plan.world.matrix[10] = -1.001f;
    plan.world.matrix[11] = -1;
    plan.world.matrix[14] = -0.1001f;
    assert(!maage_status_failed(maage_render_effect_uniforms_prepare(&plan, 1920, 1080, 3, 1536, &uniforms)));
    assert(sizeof(uniforms) == 576 && uniforms.features[0] == 1 && uniforms.splits[0] < uniforms.splits[1] && uniforms.splits[1] < uniforms.splits[2] && uniforms.splits[2] == 96);
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j) {
            sum = 0;
            for (k = 0; k < 4; ++k)
                sum += plan.world.matrix[k * 4 + j] * uniforms.inverse_view_projection[i * 4 + k];
            assert(fabs(sum - (i == j ? 1.0 : 0.0)) < 0.0001);
        }
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 4; ++j)
            center[j] = uniforms.shadows[i][j + 8] * -(uniforms.splits[i] + (i ? uniforms.splits[i - 1] : 0.1f)) * 0.5f + uniforms.shadows[i][12 + j];
        assert(fabs(center[0]) < 1 && fabs(center[1]) < 1 && center[2] > 0 && center[2] < 1);
    }
    assert(maage_render_effect_bounds_visible(bounds, identity));
    identity[12] = 10;
    assert(!maage_render_effect_bounds_visible(bounds, identity));
    plan.effect_frame.sun[3] = 0;
    plan.effect_frame.moon[3] = 0.08f;
    assert(!maage_status_failed(maage_render_effect_uniforms_prepare(&plan, 800, 600, 4, 1024, &night)));
    assert(memcmp(uniforms.shadows, night.shadows, sizeof(uniforms.shadows)));
    plan.effect_frame.weather[2] = 0;
    assert(!maage_status_failed(maage_render_effect_uniforms_prepare(&plan, 800, 600, 0, 1, &night)));
    assert(night.features[0] == 0 && night.features[3] == 0 && night.quality[1] == 0);
    memset(plan.world.matrix, 0, sizeof(plan.world.matrix));
    assert(maage_status_failed(maage_render_effect_uniforms_prepare(&plan, 800, 600, 0, 1, &night)));
    maage_shader_settings_preset(&settings, 1);
    assert(maage_status_code(maage_renderer_set_shader_effects(&renderer, &settings, &frame)) == MAAGE_STATUS_PARTIAL);
    renderer.capabilities.features = MAAGE_RENDER_CAP_WORLD_EFFECTS;
    assert(!maage_status_failed(maage_renderer_set_shader_effects(&renderer, &settings, &frame)));
    frame.weather[3] = 2;
    assert(maage_status_failed(maage_renderer_set_shader_effects(&renderer, &settings, &frame)));
}
static void check_observed_jobs(maage_distant_lod *lod, const maage_content *content) {
    maage_world_config config;
    maage_world *world = NULL;
    maage_column_update packet, *copy = NULL;
    maage_biome_id biomes[256];
    unsigned int palettes[4];
    lod_task task, *work;
    lod_record record;
    maage_job_completion completion;
    maage_status result;
    maage_job_id job;
    char path[MAAGE_PATH_CAPACITY], temporary[MAAGE_PATH_CAPACITY];
    FILE *file;
    size_t i;
    maage_block_state_id ground = maage_biome_registry_get(maage_content_biomes(content), 0)->surface;
    memset(&packet, 0, sizeof(packet));
    memset(biomes, 0, sizeof(biomes));
    packet.x = -17;
    packet.z = -32;
    packet.revision = 1;
    packet.biomes = biomes;
    for (i = 0; i < 4; ++i) {
        palettes[i] = (unsigned int) ground;
        packet.sections[i].palette_count = 1;
        packet.sections[i].palette = &palettes[i];
    }
    maage_world_config_init(&config);
    config.dimension = maage_dimension_get(maage_content_dimensions(content), maage_dimension_primary(maage_content_dimensions(content)));
    config.registry = maage_content_blocks(content);
    config.biomes = maage_content_biomes(content);
    config.items = maage_content_items(content);
    config.execution = *maage_content_execution(content);
    config.presentation_only = 1;
    config.build_meshes = 0;
    config.simulation_distance = 0;
    config.worker_count = 1;
    config.job_capacity = 4;
    config.cache_capacity = 32;
    config.render_distance = 2;
    assert(!maage_status_failed(maage_world_create(&config, &world, NULL)));
    assert(!maage_status_failed(maage_world_apply_column(world, &packet)));
    assert(!maage_status_failed(maage_world_copy_column(world, -17, -32, &copy)));
    assert(copy && copy->revision == 1 && copy->biomes && !copy->unload);
    maage_column_update_destroy(copy);
    assert(maage_status_code(maage_distant_lod_observe(lod, world, -17, -32)) == MAAGE_STATUS_SUCCESS);
    assert(!maage_status_failed(maage_jobs_wait(lod->jobs)));
    assert(maage_status_code(maage_jobs_poll(lod->jobs, &completion)) == MAAGE_STATUS_SUCCESS);
    if (maage_status_failed(completion.result)) maage_log_trace_status(MAAGE_LOG_ERROR, "maage.client.lod", completion.result, &completion.trace, "Offline observation failed.");
    assert(!completion.cancelled && !maage_status_failed(completion.result));
    --lod->active;
    lod_task_free(completion.context);
    memset(&task, 0, sizeof(task));
    task.owner = lod;
    task.appearance = lod->appearance;
    assert(lod_load(&task, -17, -32, 1, 1, &record));
    assert(record.samples[0].height == 64 && record.revision == 1);
    /* Atomic failure recovery: an interrupted temporary file cannot replace
     * the previously committed observation. Corruption fails validation. */
    assert(lod_path(lod, -17, -32, 1, 1, 1, temporary));
    file = maage_storage_open(temporary, "wb");
    assert(file);
    fputs("partial", file);
    fclose(file);
    assert(lod_load(&task, -17, -32, 1, 1, &record));
    assert(lod_path(lod, -17, -32, 1, 1, 0, path));
    file = maage_storage_open(path, "wb");
    assert(file);
    fputs("corrupt", file);
    fclose(file);
    assert(!lod_load(&task, -17, -32, 1, 1, &record));
    /* A later confirmed edit replaces that derivative through the same worker. */
    packet.revision = 2;
    packet.sections[3].palette_count = 0;
    packet.sections[3].palette = NULL;
    assert(!maage_status_failed(maage_world_apply_column(world, &packet)));
    assert(maage_status_code(maage_distant_lod_observe(lod, world, -17, -32)) == MAAGE_STATUS_SUCCESS);
    assert(!maage_status_failed(maage_jobs_wait(lod->jobs)));
    assert(maage_status_code(maage_jobs_poll(lod->jobs, &completion)) == MAAGE_STATUS_SUCCESS);
    assert(!maage_status_failed(completion.result));
    --lod->active;
    lod_task_free(completion.context);
    assert(lod_load(&task, -17, -32, 1, 1, &record));
    assert(record.samples[0].height == 48 && record.revision == 2);
    /* Execute actual generation/preparation, then fence that result with an
     * epoch change. No GPU adapter or fake upload implementation is involved. */
    work = (lod_task *) calloc(1, sizeof(*work));
    assert(work);
    work->owner = lod;
    work->appearance = lod->appearance;
    ++work->appearance->references;
    work->x = -17;
    work->z = -32;
    work->span = 1;
    work->quality = 0;
    work->epoch = lod->epoch;
    assert(!maage_status_failed(maage_jobs_submit(lod->jobs, 0, lod_run, work, &job, NULL)));
    ++lod->active;
    assert(!maage_status_failed(maage_jobs_wait(lod->jobs)));
    assert(work->prepared && work->vertices && work->indices);
    assert(work->vertices[0].position[1] == 48.0f);
    maage_distant_lod_invalidate_gpu(lod);
    assert(work->epoch != lod->epoch);
    {
        unsigned long stale = lod->stats.stale;
        result = lod_poll(lod, NULL, NULL);
        assert(!maage_status_failed(result) && lod->stats.stale == stale + 1 && !lod->active && !lod->pending);
    }
    result = maage_world_destroy(world);
    assert(!maage_status_failed(result));
}
int main(int argc, char **argv) {
    maage_content_config config;
    maage_content *content = NULL;
    maage_distant_lod *lod;
    maage_world_seed seed;
    maage_generation_policy policy;
    int far_radius, quality, near_radius, position;
    size_t old_capacity;
    maage_application_content_config(&config);
    config.execution.side = MAAGE_EXECUTION_CLIENT;
    config.execution.capabilities = MAAGE_EXECUTION_PRESENTATION;
    assert(!maage_status_failed(maage_content_create(&config, &content, NULL)));
    memset(&seed, 0, sizeof(seed));
    maage_generation_policy_default(&policy);
    /* Constructor does no disk I/O; tests do not run sampler/cache write jobs. */
    lod = maage_distant_lod_create(argc > 1 ? argv[1] : ".", "12345678-1234-4234-8234-123456789abc", maage_dimension_primary(maage_content_dimensions(content)), seed, &policy, maage_content_blocks(content), maage_content_biomes(content));
    assert(lod);
    check_cache(lod);
    check_frame_plan();
    check_effect_material_pack();
    check_shader_effects(argc > 1 ? argv[1] : NULL);
    if (argc > 1) check_observed_jobs(lod, content);
    for (near_radius = 4; near_radius <= 32; near_radius += 14)
        for (far_radius = 16; far_radius <= 128; far_radius *= 2)
            for (quality = 0; quality < 3; ++quality) {
                if (far_radius <= near_radius + 2) continue;
                for (position = -17; position <= 1; position += 9)
                    check_plan(lod, position, -position - 1, near_radius, far_radius, quality);
            }
    check_plan(lod, MAAGE_WORLD_LIMIT / 16 - 10, -MAAGE_WORLD_LIMIT / 16 + 10, 4, 32, 1);
    check_plan(lod, -1, -1, 12, 128, 1);
    old_capacity = lod->capacity;
    check_plan(lod, 0, 0, 12, 16, 1);
    assert(lod->capacity == old_capacity); /* Hysteresis retains CPU allocations. */
    lod_plan(lod, 0, 0, 12, 12, 1);
    assert(!lod->root_count);
    lod_plan(lod, 0, 0, 12, 128, 1);
    assert(lod->root_count);
    maage_distant_lod_invalidate_gpu(lod);
    assert(!lod->planned);
    assert(!maage_status_failed(maage_distant_lod_destroy(lod)));
    lod = maage_distant_lod_create(argc > 1 ? argv[1] : ".", "12345678-1234-4234-8234-123456789abc", maage_dimension_primary(maage_content_dimensions(content)), seed, &policy, maage_content_blocks(content), maage_content_biomes(content));
    assert(lod);
    lod->cpu_budget = 1024;
    lod_plan(lod, 0, 0, 12, 128, 1);
    assert(lod->overflow && lod->capacity == maage_distant_lod_capacity(NULL));
    assert(!maage_status_failed(maage_distant_lod_destroy(lod)));
    maage_content_destroy(content);
    puts("PASS: shader presets/INI round trip/frame validation/CSM transforms; shader plans; cache integrity/identity; observed edits and atomic publication; real Jobs generation; epoch fencing; negative-coordinate multiresolution coverage and distance changes.");
    return 0;
}

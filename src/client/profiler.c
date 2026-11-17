#include "profiler.h"
#include "maage/storage.h"
#include "maage/log.h"
#include "maage/time.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* A new file per run avoids mixing revisions or losing a prior capture.
 * Storage is always under the Launcher's validated instance directory. */
static int maage_profile_compare(const void *left, const void *right) {
    double a = *(const double *) left, b = *(const double *) right;
    return a < b ? -1 : a > b ? 1 : 0;
}
static double maage_profile_duration(double a, double b) {
    return a >= 0 && b >= a ? (b - a) * 1000.0 : -1.0;
}
void maage_client_profiler_open(maage_client_profiler *p, const char *directory) {
    const char *enabled = getenv("MAAGE_PROFILE");
    char path[MAAGE_PATH_CAPACITY], name[80];
    unsigned int suffix;
    time_t epoch;
    maage_status result;
    memset(p, 0, sizeof(*p));
    if (!enabled || strcmp(enabled, "1") || !directory) return;
    epoch = time(NULL);
    for (suffix = 0; suffix < 100; ++suffix) {
        sprintf(name, "profile-%lu-%u.csv", (unsigned long) epoch, suffix);
        result = maage_storage_join(path, sizeof(path), directory, name);
        if (maage_status_failed(result)) return;
        { FILE *existing = maage_storage_open(path, "rb");
          if (existing) { fclose(existing); continue; } }
        p->file = maage_storage_open(path, "wb");
        if (p->file) break;
        maage_log_warning("maage.client.profiler", "Cannot open profiler output: %s", path);
        return;
    }
    if (!p->file) {
        maage_log_warning("maage.client.profiler", "Profiler output filename collision limit reached.");
        return;
    }
    setvbuf(p->file, p->io_buffer, _IOFBF, sizeof(p->io_buffer));
    if (fputs("frame,tick,interval_ms,work_ms,input_ms,scene_ms,setup_ms,renderer_ms,upload_ms,other_ms,loaded,pending,running_jobs,uploaded_meshes,upload_budget_ms,presented,lod_tiles,lod_jobs,lod_upload,lod_hits,lod_generated,lod_stale,lod_worker_ms,lod_gpu_estimate_bytes,lod_budget_limited,shaders,shader_quality,shader_exposure,effects_supported,shadows,shadow_quality,shadow_distance,dynamic_lights,light_quality,selected_lights,waving,wind,god_rays,ray_quality,sky,water,water_quality,underwater\n", p->file) < 0) {
        fclose(p->file);
        p->file = NULL;
        return;
    }
    p->active = 1;
    p->window_start = maage_time_monotonic_seconds();
    maage_log_info("maage.client.profiler", "Profiler enabled. Per-frame timings: %s (CPU wall timings; not GPU execution times).", path);
}
void maage_client_profiler_record(maage_client_profiler *p, const maage_client_profile_marks *m, double interval, unsigned long tick,
    unsigned loaded, unsigned pending, unsigned running_jobs, unsigned uploaded_meshes, double upload_budget_ms, int presented) {
    double work, input, scene, setup, render, upload, other, now;
    double sorted[MAAGE_CLIENT_PROFILE_WINDOW];
    unsigned i;
    if (!p->active) return;
    work = maage_profile_duration(m->begin, m->end);
    input = maage_profile_duration(m->begin, m->input);
    scene = maage_profile_duration(m->input, m->scene);
    setup = maage_profile_duration(m->scene, m->draw_begin);
    render = maage_profile_duration(m->draw_begin, m->draw_end);
    upload = maage_profile_duration(m->upload_begin, m->end);
    if (work < 0 || input < 0 || scene < 0 || setup < 0 || render < 0 || upload < 0) return;
    other = work - input - scene - setup - render - upload;
    if (other < 0) other = 0;
    p->last_frame_ms = work;
    p->last_input_ms = input;
    p->last_scene_ms = scene;
    p->last_setup_ms = setup;
    p->last_render_ms = render;
    p->last_upload_ms = upload;
    p->samples[p->sample_cursor] = work;
    p->sample_cursor = (p->sample_cursor + 1U) % MAAGE_CLIENT_PROFILE_WINDOW;
    if (p->sample_count < MAAGE_CLIENT_PROFILE_WINDOW) ++p->sample_count;
    if (work > p->max_ms) p->max_ms = work;
    if (work > 33.333) ++p->hitches;
    ++p->frames;
    if (fprintf(p->file, "%lu,%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u,%.3f,%d,%u,%u,%u,%lu,%lu,%lu,%.3f,%.0f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d\n",
                p->frames, tick, interval * 1000.0, work, input, scene, setup, render, upload, other,
                loaded, pending, running_jobs, uploaded_meshes, upload_budget_ms, presented, p->lod.resident, p->lod.active, p->lod.publication_backlog, p->lod.cache_hits, p->lod.generated, p->lod.stale, p->lod.generation_ms, (double) p->lod.gpu_bytes_estimate, p->lod.budget_limited, p->shaders_enabled, p->shader_quality, p->shader_exposure, p->shader_effects_supported, p->effects.shadows, p->effects.shadow_quality, p->effects.shadow_distance, p->effects.dynamic_lights, p->effects.light_quality, p->shader_lights, p->effects.waving, p->effects.wind, p->effects.god_rays, p->effects.ray_quality, p->effects.sky, p->effects.water, p->effects.water_quality, p->effects.underwater) < 0) {
        maage_log_warning("maage.client.profiler", "Profiling output write failed; disabling capture.");
        maage_client_profiler_close(p);
        return;
    }
    now = maage_time_monotonic_seconds();
    if (now - p->window_start >= 5.0) {
        memcpy(sorted, p->samples, p->sample_count * sizeof(double));
        qsort(sorted, p->sample_count, sizeof(double), maage_profile_compare);
        i = (unsigned) ((p->sample_count * 95U + 99U) / 100U);
        p->p95_ms = p->sample_count ? sorted[i ? i - 1 : 0] : 0;
        maage_log_info("maage.client.profiler", "frames=%lu recent_p95_work=%.2fms max_work=%.2fms hitches_over_33ms=%lu last_input=%.2fms scene=%.2fms setup=%.2fms renderer=%.2fms upload=%.2fms",
            p->frames, p->p95_ms, p->max_ms, p->hitches, input, scene, setup, render, upload);
        if (fflush(p->file)) {
            maage_log_warning("maage.client.profiler", "Profiler output flush failed; disabling capture.");
            maage_client_profiler_close(p);
            return;
        }
        p->window_start = now;
    }
}
void maage_client_profiler_close(maage_client_profiler *p) {
    if (!p->file) return;
    if (fclose(p->file)) maage_log_warning("maage.client.profiler", "Profiler CSV close failed; capture may be incomplete.");
    p->file = NULL;
    p->active = 0;
}

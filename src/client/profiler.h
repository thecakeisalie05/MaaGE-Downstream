#ifndef MAAGE_CLIENT_PROFILER_H
#define MAAGE_CLIENT_PROFILER_H
#include <stdio.h>
#include "distant_lod.h"
#define MAAGE_CLIENT_PROFILE_WINDOW 512
typedef struct maage_client_profile_marks {
    double begin, input, scene, draw_begin, draw_end, upload_begin, end;
} maage_client_profile_marks;
typedef struct maage_client_profiler {
    FILE *file;
    char io_buffer[16384];
    double samples[MAAGE_CLIENT_PROFILE_WINDOW];
    unsigned sample_count, sample_cursor;
    unsigned long frames, hitches;
    double window_start, last_frame_ms, p95_ms, max_ms;
    double last_input_ms, last_scene_ms, last_setup_ms, last_render_ms, last_upload_ms;
    maage_distant_lod_stats lod;
    int shaders_enabled, shader_quality, shader_exposure;
    maage_shader_settings effects;
    unsigned int shader_lights;
    int shader_effects_supported;
    int active;
} maage_client_profiler;
/* Optional and owner-thread-only. Timestamps measure elapsed CPU thread wall time;
 * renderer submission may block, but these are NOT GPU hardware timestamps. */
void maage_client_profiler_open(maage_client_profiler *profiler, const char *directory);
void maage_client_profiler_record(maage_client_profiler *profiler, const maage_client_profile_marks *marks, double frame_interval,
    unsigned long server_tick, unsigned loaded, unsigned pending, unsigned running_jobs,
    unsigned uploaded_meshes, double upload_budget_ms, int presented);
void maage_client_profiler_close(maage_client_profiler *profiler);
#endif

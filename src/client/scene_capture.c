#include "scene_capture.h"
#include "maage/storage.h"
#include "maage/client_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
static int scene_numbers(const char *text, double *values, unsigned int count) {
    char *end;
    unsigned int i;
    for (i = 0; i < count; ++i) {
        errno = 0;
        values[i] = strtod(text, &end);
        if (errno || end == text || !(values[i] >= -30000000 && values[i] <= 30000000)) return 0;
        if (i + 1 < count) {
            if (*end != ',') return 0;
            text = end + 1;
        } else if (*end) return 0;
    }
    return 1;
}
maage_status maage_client_scene_parse(int argc, char *const argv[], maage_client_scene_capture *scene) {
    int i, extra = 0;
    double value[3];
    memset(scene, 0, sizeof(*scene));
    scene->width = 1280;
    scene->height = 720;
    scene->timeout = 180;
    scene->daytime = -1;
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strncmp(arg, "--render-scene=", 15)) {
            if (scene->path || !arg[15] || strlen(arg + 15) >= MAAGE_PATH_CAPACITY - 8) goto invalid;
            scene->path = arg + 15;
        } else if (!strncmp(arg, "--scene-look=", 13)) {
            if (!scene_numbers(arg + 13, scene->look, 2) || scene->look[1] < -89 || scene->look[1] > 89) goto invalid;
            scene->has_look = extra = 1;
        } else if (!strncmp(arg, "--scene-eye=", 12)) {
            if (!scene_numbers(arg + 12, scene->eye, 3) || scene->eye[1] < 0 || scene->eye[1] > 256) goto invalid;
            scene->has_eye = extra = 1;
        } else if (!strncmp(arg, "--scene-size=", 13)) {
            char *end;
            long width, height;
            errno = 0;
            width = strtol(arg + 13, &end, 10);
            if (errno || end == arg + 13 || *end != 'x' || width < 64 || width > 8192) goto invalid;
            arg = end + 1;
            errno = 0;
            height = strtol(arg, &end, 10);
            if (errno || end == arg || *end || height < 64 || height > 8192) goto invalid;
            scene->width = (int) width;
            scene->height = (int) height;
            extra = 1;
        } else if (!strncmp(arg, "--scene-clock=", 14)) {
            if (!scene_numbers(arg + 14, value, 1) || value[0] < 0 || value[0] > 36000) goto invalid;
            scene->clock = value[0];
            extra = 1;
        } else if (!strncmp(arg, "--scene-daytime=", 16)) {
            if (!scene_numbers(arg + 16, value, 1) || value[0] < 0 || value[0] >= 24000 || floor(value[0]) != value[0]) goto invalid;
            scene->daytime = (int) value[0];
            extra = 1;
        } else if (!strncmp(arg, "--scene-wait=", 13)) {
            if (!scene_numbers(arg + 13, value, 1) || value[0] < 1 || value[0] > 3600) goto invalid;
            scene->timeout = value[0];
            extra = 1;
        } else if (!strcmp(arg, "--scene-ui")) scene->ui = extra = 1;
        else if (!strcmp(arg, "--render-scene") || !strncmp(arg, "--scene-", 8)) goto invalid;
    }
    if (extra && !scene->path) goto invalid;
    if (scene->path) {
        size_t length = strlen(scene->path);
        if (length < 5 || strcmp(scene->path + length - 4, ".png")) goto invalid;
    }
    return maage_status_make(MAAGE_STATUS_SUCCESS, 0, MAAGE_OPERATION_PARSE, 0);
invalid:
    return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_PARSE, 0);
}
maage_status maage_client_scene_save(const maage_client_scene_capture *scene, maage_renderer *renderer, const maage_client_options *options, const maage_session_snapshot *snapshot, const char *world, const double eye[3], double yaw, double pitch, maage_status_trace *trace) {
    maage_image image;
    maage_render_capabilities caps;
    maage_status result;
    char temporary[MAAGE_PATH_CAPACITY], metadata[MAAGE_PATH_CAPACITY];
    FILE *file;
    int failed, width, height;
    memset(&image, 0, sizeof(image));
    result = maage_renderer_read_frame(renderer, &image, trace);
    if (maage_status_code(result) != MAAGE_STATUS_SUCCESS) return result;
    sprintf(temporary, "%s.tmp", scene->path);
    width = image.width;
    height = image.height;
    result = maage_image_write_png(temporary, &image, trace);
    maage_image_destroy(&image);
    if (!maage_status_failed(result)) result = maage_storage_replace(temporary, scene->path);
    if (maage_status_failed(result)) return result;
    sprintf(metadata, "%s.txt", scene->path);
    file = maage_storage_open(metadata, "wb");
    if (!file) return maage_status_from_errno(errno, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE);
    maage_renderer_get_capabilities(renderer, &caps);
    failed = fprintf(file, "MaaGE %s\nworld=%s\nrenderer=%s\ndevice=%s\nsize=%dx%d\ncapabilities=0x%08lX\neye=%.9g,%.9g,%.9g\nlook_degrees=%.9g,%.9g\nvisual_clock=%.9g\ndaytime=%lu\nrain=%.6g\nthunder=%.6g\nshaders=%d\npreset=%d\nexposure=%d\nrender_distance=%d\nlod=%d\nlod_distance=%d\nlod_quality=%d\nfov_degrees=%d\n", MAAGE_CLIENT_VERSION, world, maage_render_backend_name(maage_renderer_get_backend(renderer)), maage_renderer_device_name(renderer), width, height, (unsigned long) caps.features, eye[0], eye[1], eye[2], yaw * 57.29577951308232, pitch * 57.29577951308232, scene->clock, snapshot->environment.day_tick, snapshot->environment.rain_intensity, snapshot->environment.thunder_intensity, options->shaders_enabled, options->shader_quality, options->shader_exposure, options->render_distance, options->lod_enabled, options->lod_distance, options->lod_quality, options->fov_degrees) < 0;
    if (fprintf(file, "dimension=%lu\nmoon_phase=%lu\nleaves=%d\nbiome_blend=%d\nview_bobbing=%d\ngui_scale=%d\ninclude_ui=%d\n", (unsigned long) snapshot->dimension, snapshot->environment.day_low & 7UL, (int) options->leaves, (int) options->biome_blend, options->view_bobbing, options->gui_scale, scene->ui) < 0) failed = 1;
#define MAAGE_SCENE_SETTING(name, minimum, maximum) if (fprintf(file, #name "=%d\n", options->shader_effects.name) < 0) failed = 1;
    MAAGE_SHADER_CONTROLS(MAAGE_SCENE_SETTING)
#undef MAAGE_SCENE_SETTING
    result = failed ? maage_status_make(MAAGE_STATUS_IO_ERROR, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE, 0) : maage_storage_sync(file);
    if (fclose(file) && !maage_status_failed(result)) result = maage_status_make(MAAGE_STATUS_IO_ERROR, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_CLOSE, 0);
    return result;
}

#ifndef MAAGE_CLIENT_SCENE_CAPTURE_H
#define MAAGE_CLIENT_SCENE_CAPTURE_H
#include "options.h"
#include "maage/render.h"
#include "maage/session_protocol.h"
typedef struct maage_client_scene_capture {
    const char *path;
    int width, height, ui, has_look, has_eye, daytime, captured;
    double look[2], eye[3], clock, timeout, started;
    unsigned int settled;
} maage_client_scene_capture;
maage_status maage_client_scene_parse(int argc, char *const argv[], maage_client_scene_capture *scene);
maage_status maage_client_scene_save(const maage_client_scene_capture *scene, maage_renderer *renderer, const maage_client_options *options, const maage_session_snapshot *snapshot, const char *world, const double eye[3], double yaw, double pitch, maage_status_trace *trace);
#endif

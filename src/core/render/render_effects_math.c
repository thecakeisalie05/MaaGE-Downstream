#include "render_effects_internal.h"
#include <math.h>
#include <string.h>
#include <float.h>
static double effect_dot(const double a[3], const double b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static void effect_normalize(double value[3]) {
    double length = sqrt(effect_dot(value, value));
    unsigned int i;
    if (length < 0.000001) length = 1;
    for (i = 0; i < 3; ++i)
        value[i] /= length;
}
static void effect_cross(const double a[3], const double b[3], double result[3]) {
    result[0] = a[1] * b[2] - a[2] * b[1];
    result[1] = a[2] * b[0] - a[0] * b[2];
    result[2] = a[0] * b[1] - a[1] * b[0];
}
static int effect_inverse(const float input[16], float output[16]) {
    double rows[4][8], value, maximum, temporary;
    unsigned int i, j, column, pivot;
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 8; ++j)
            rows[i][j] = j < 4 ? input[j * 4 + i] : (j - 4 == i ? 1 : 0);
    for (column = 0; column < 4; ++column) {
        pivot = column;
        maximum = fabs(rows[pivot][column]);
        for (i = column + 1; i < 4; ++i)
            if (fabs(rows[i][column]) > maximum) {
                pivot = i;
                maximum = fabs(rows[i][column]);
            }
        if (!(maximum > 0.000000000001)) return 0;
        for (j = 0; j < 8; ++j) {
            temporary = rows[column][j];
            rows[column][j] = rows[pivot][j];
            rows[pivot][j] = temporary;
        }
        value = rows[column][column];
        for (j = 0; j < 8; ++j)
            rows[column][j] /= value;
        for (i = 0; i < 4; ++i)
            if (i != column) {
                value = rows[i][column];
                for (j = 0; j < 8; ++j)
                    rows[i][j] -= rows[column][j] * value;
            }
    }
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j) {
            if (!(rows[i][j + 4] >= -FLT_MAX && rows[i][j + 4] <= FLT_MAX)) return 0;
            output[j * 4 + i] = (float) rows[i][j + 4];
        }
    return 1;
}
static void effect_unproject(const float inverse[16], double x, double y, double z, double output[3]) {
    double w = inverse[3] * x + inverse[7] * y + inverse[11] * z + inverse[15];
    unsigned int i;
    if (fabs(w) < 0.0000001) w = w < 0 ? -0.0000001 : 0.0000001;
    for (i = 0; i < 3; ++i)
        output[i] = (inverse[i] * x + inverse[4 + i] * y + inverse[8 + i] * z + inverse[12 + i]) / w;
}
maage_status maage_render_effect_uniforms_prepare(const maage_render_frame_plan *plan, unsigned int width, unsigned int height, unsigned int cascades, unsigned int resolution, maage_effect_uniforms *uniforms) {
    static const double fractions[4][4] = {{1, 1, 1, 1}, {0.22, 1, 1, 1}, {0.12, 0.4, 1, 1}, {0.08, 0.24, 0.5, 1}};
    double eye[3], forward[3], light[3], right[3], up[3] = {0, 1, 0}, rays[4][3], points[8][3], center[3], delta[3];
    double previous = 0.1, split, depth, radius, texel, snapped_x, snapped_y, factor, direct;
    unsigned int i, j, k, cascade;
    const maage_shader_frame *frame;
    const maage_shader_settings *settings;
    if (!plan || !uniforms || !width || !height || cascades > 4 || !resolution) return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_RENDER, MAAGE_OPERATION_UPDATE, 0);
    memset(uniforms, 0, sizeof(*uniforms));
    frame = &plan->effect_frame;
    settings = &plan->effects;
    memcpy(uniforms->view_projection, plan->world.matrix, sizeof(uniforms->view_projection));
    if (!effect_inverse(plan->world.matrix, uniforms->inverse_view_projection)) return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_RENDER, MAAGE_OPERATION_UPDATE, 0);
    for (i = 0; i < 3; ++i) {
        eye[i] = frame->camera[i];
        uniforms->camera_time[i] = frame->camera[i];
    }
    uniforms->camera_time[3] = frame->time_seconds;
    memcpy(uniforms->sun, frame->sun, sizeof(frame->sun));
    memcpy(uniforms->moon, frame->moon, sizeof(frame->moon));
    memcpy(uniforms->sun_color, frame->sun_color, sizeof(frame->sun_color));
    memcpy(uniforms->moon_color, frame->moon_color, sizeof(frame->moon_color));
    memcpy(uniforms->weather, frame->weather, sizeof(frame->weather));
    uniforms->viewport[0] = (float) width;
    uniforms->viewport[1] = (float) height;
    uniforms->viewport[2] = 1.0f / width;
    uniforms->viewport[3] = 1.0f / height;
    uniforms->wind[0] = settings->wind * 0.01f;
    uniforms->wind[1] = (float) settings->shadow_distance;
    uniforms->wind[2] = (float) settings->water;
    uniforms->wind[3] = (float) cascades;
    uniforms->features[0] = settings->shadows && cascades && frame->weather[2] > 0 ? 1.0f : 0;
    uniforms->features[1] = settings->dynamic_lights ? (float) frame->light_count : 0;
    uniforms->features[2] = (float) settings->waving;
    uniforms->features[3] = settings->sky && frame->weather[2] > 0 ? 1.0f : 0;
    uniforms->quality[0] = (float) ((settings->shadow_quality + 2) * (settings->shadow_quality + 2));
    uniforms->quality[1] = settings->god_rays && frame->weather[2] > 0 ? (float) (8 + settings->ray_quality * 8) : 0;
    uniforms->quality[2] = (float) settings->water_quality;
    uniforms->quality[3] = (float) settings->underwater;
    for (i = 0; i < 3; ++i)
        uniforms->ambient[i] = plan->world.fog[i];
    uniforms->ambient[3] = plan->world.parameters[0];
    effect_unproject(uniforms->inverse_view_projection, 0, 0, 1, forward);
    for (i = 0; i < 3; ++i)
        forward[i] -= eye[i];
    effect_normalize(forward);
    direct = frame->sun[3] + frame->moon[3];
    for (i = 0; i < 3; ++i)
        light[i] = frame->sun[3] >= frame->moon[3] ? frame->sun[i] : frame->moon[i];
    if (effect_dot(forward, light) < -0.1 || direct < 0.005) uniforms->quality[1] = 0;
    for (j = 0; j < 4; ++j) {
        effect_unproject(uniforms->inverse_view_projection, j & 1 ? 1 : -1, j & 2 ? 1 : -1, 1, rays[j]);
        for (i = 0; i < 3; ++i)
            rays[j][i] -= eye[i];
    }
    for (i = 0; i < 3; ++i)
        light[i] = frame->sun[3] >= frame->moon[3] ? frame->sun[i] : frame->moon[i];
    effect_normalize(light);
    if (fabs(light[1]) > 0.95) {
        up[1] = 0;
        up[2] = 1;
    }
    effect_cross(light, up, right);
    effect_normalize(right);
    effect_cross(right, light, up);
    effect_normalize(up);
    for (cascade = 0; cascade < cascades; ++cascade) {
        split = fractions[cascades - 1][cascade] * settings->shadow_distance;
        for (i = 0; i < 3; ++i)
            center[i] = 0;
        for (j = 0; j < 8; ++j) {
            factor = effect_dot(rays[j % 4], forward);
            if (factor < 0.00001) factor = 0.00001;
            factor = (j < 4 ? previous : split) / factor;
            for (i = 0; i < 3; ++i) {
                points[j][i] = eye[i] + rays[j % 4][i] * factor;
                center[i] += points[j][i] / 8;
            }
        }
        radius = 0;
        for (j = 0; j < 8; ++j) {
            for (i = 0; i < 3; ++i)
                delta[i] = points[j][i] - center[i];
            factor = sqrt(effect_dot(delta, delta));
            if (factor > radius) radius = factor;
        }
        radius = ceil(radius * 16) / 16;
        if (radius < 1) radius = 1;
        texel = radius * 2 / resolution;
        snapped_x = floor(effect_dot(center, right) / texel + 0.5) * texel;
        snapped_y = floor(effect_dot(center, up) / texel + 0.5) * texel;
        depth = radius + 256;
        for (k = 0; k < 3; ++k) {
            uniforms->shadows[cascade][k * 4] = (float) (right[k] / radius);
            uniforms->shadows[cascade][k * 4 + 1] = (float) (up[k] / radius);
            uniforms->shadows[cascade][k * 4 + 2] = (float) (-light[k] / (depth * 2));
        }
        uniforms->shadows[cascade][12] = (float) (-snapped_x / radius);
        uniforms->shadows[cascade][13] = (float) (-snapped_y / radius);
        uniforms->shadows[cascade][14] = (float) (0.5 + effect_dot(center, light) / (depth * 2));
        uniforms->shadows[cascade][15] = 1;
        uniforms->splits[cascade] = (float) split;
        previous = split;
    }
    return maage_status_make(MAAGE_STATUS_SUCCESS, 0, MAAGE_OPERATION_UPDATE, 0);
}
int maage_render_effect_bounds_visible(const float bounds[6], const float matrix[16]) {
    unsigned int plane, corner, axis;
    float clip[4], point[3], distance;
    int all_outside;
    for (plane = 0; plane < 6; ++plane) {
        all_outside = 1;
        for (corner = 0; corner < 8; ++corner) {
            for (axis = 0; axis < 3; ++axis)
                point[axis] = bounds[axis + (corner & (1U << axis) ? 3 : 0)] + (corner & (1U << axis) ? 0.5f : -0.5f);
            for (axis = 0; axis < 4; ++axis)
                clip[axis] = matrix[axis] * point[0] + matrix[4 + axis] * point[1] + matrix[8 + axis] * point[2] + matrix[12 + axis];
            distance = plane == 0 ? clip[0] + clip[3] : plane == 1 ? clip[3] - clip[0]
                                                    : plane == 2   ? clip[1] + clip[3]
                                                    : plane == 3   ? clip[3] - clip[1]
                                                    : plane == 4   ? clip[2]
                                                                   : clip[3] - clip[2];
            if (distance >= 0) {
                all_outside = 0;
                break;
            }
        }
        if (all_outside) return 0;
    }
    return 1;
}

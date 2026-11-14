#include "screenshot.h"
#include "maage/storage.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <png.h>
#include <stdlib.h>
/* Capture only the visible Client area of the window owned by this process.
 * Never BitBlt an unrelated foreground window, even during Alt+Tab. */
maage_status maage_client_screenshot_save(const char *directory, const char *session_id,
    unsigned int sequence, char *saved, size_t capacity) {
    HWND hwnd;
    DWORD owner;
    RECT client;
    POINT origin;
    HDC desktop = NULL, compatible = NULL;
    HBITMAP dib = NULL, old_bitmap = NULL;
    BITMAPINFO info;
    unsigned char *pixel_bytes = NULL, *rgba = NULL, *source;
    png_image image;
    size_t pixels, i;
    int width, height, status, captured = 0;
    char name[128], path[MAAGE_PATH_CAPACITY], token[65];
    maage_status result = maage_status_make(MAAGE_STATUS_IO_ERROR, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE, 0);
    if (saved && capacity) saved[0] = 0;
    if (!directory || !saved || !capacity || !sequence) return
        maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE, 0);
    token[0] = 0;
    if (session_id && *session_id) {
        size_t count = strlen(session_id);
        if (count > 64) return maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE, 0);
        for (i = 0; i < count; ++i) {
            char c = session_id[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '-')) return
                maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE, 0);
        }
        strcpy(token, session_id);
    } else strcpy(token, "manual");
    if (snprintf(name, sizeof(name), "screenshot-%s-%04u.png", token, sequence) < 0) return result;
    result = maage_storage_join(path, sizeof(path), directory, name);
    if (maage_status_failed(result)) return result;
    hwnd = GetForegroundWindow();
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return
        maage_status_make(MAAGE_STATUS_INVALID_STATE, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE, 0);
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner != GetCurrentProcessId()) return
        maage_status_make(MAAGE_STATUS_INVALID_STATE, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE, 0);
    if (!GetClientRect(hwnd, &client)) return result;
    width = client.right - client.left;
    height = client.bottom - client.top;
    if (width < 1 || height < 1 || width > 8192 || height > 8192 ||
        (size_t) width * (size_t) height > 40000000U) return
        maage_status_make(MAAGE_STATUS_INVALID_ARGUMENT, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE, 0);
    pixels = (size_t) width * (size_t) height;
    origin.x = origin.y = 0;
    if (!ClientToScreen(hwnd, &origin)) return result;
    desktop = GetDC(NULL);
    if (!desktop) goto done;
    compatible = CreateCompatibleDC(desktop);
    if (!compatible) goto done;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; /* top-to-bottom rows */
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(compatible, &info, DIB_RGB_COLORS, (void **) &pixel_bytes, NULL, 0);
    if (!dib || !pixel_bytes) goto done;
    old_bitmap = (HBITMAP) SelectObject(compatible, dib);
    if (!old_bitmap) goto done;
    if (!BitBlt(compatible, 0, 0, width, height, desktop, origin.x, origin.y, SRCCOPY | CAPTUREBLT)) goto done;
    rgba = (unsigned char *) malloc(pixels * 4U);
    if (!rgba) {
        result = maage_status_make(MAAGE_STATUS_ALLOCATION_FAILED, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE, 0);
        goto done;
    }
    for (i = 0; i < pixels; ++i) {
        source = pixel_bytes + i * 4U;
        rgba[i * 4U] = source[2];
        rgba[i * 4U + 1U] = source[1];
        rgba[i * 4U + 2U] = source[0];
        rgba[i * 4U + 3U] = 255;
    }
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = (png_uint_32) width;
    image.height = (png_uint_32) height;
    image.format = PNG_FORMAT_RGBA;
    status = png_image_write_to_file(&image, path, 0, rgba, 0, NULL);
    if (status) {
        if (strlen(path) < capacity) {
            strcpy(saved, path);
            captured = 1;
        } else {
            DeleteFileA(path);
            result = maage_status_make(MAAGE_STATUS_BUFFER_OVERFLOW, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE, 0);
        }
    }
done:
    free(rgba);
    if (old_bitmap) SelectObject(compatible, old_bitmap);
    if (dib) DeleteObject(dib);
    if (compatible) DeleteDC(compatible);
    if (desktop) ReleaseDC(NULL, desktop);
    return captured ? maage_status_make(MAAGE_STATUS_SUCCESS, 0, MAAGE_OPERATION_WRITE, 0) : result;
}
#else
maage_status maage_client_screenshot_save(const char *directory, const char *session_id,
    unsigned int sequence, char *saved, size_t capacity) {
    (void) directory; (void) session_id; (void) sequence;
    if (saved && capacity) saved[0] = 0;
    return maage_status_make(MAAGE_STATUS_NOT_IMPLEMENTED, MAAGE_CONTEXT_CLIENT, MAAGE_OPERATION_WRITE, 0);
}
#endif

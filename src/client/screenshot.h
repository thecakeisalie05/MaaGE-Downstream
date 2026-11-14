#ifndef MAAGE_CLIENT_SCREENSHOT_H
#define MAAGE_CLIENT_SCREENSHOT_H
#include "maage/status.h"
/* Native window capture. The Windows adapter samples the desktop compositor,
 * so the game window must be visible and unobstructed. The session identifier
 * is a non-secret ASCII token supplied by the development launch workflow. */
maage_status maage_client_screenshot_save(const char *directory, const char *session_id,
    unsigned int sequence, char *saved, size_t capacity);
#endif

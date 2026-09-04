#ifndef GOLDENS_SCENE_CAPTURE_H
#define GOLDENS_SCENE_CAPTURE_H

#include <windows.h>
#include <wincodec.h>

#include "image_io.h"

typedef enum {
    GOLDEN_SCENE_CAPTURE_OK,
    GOLDEN_SCENE_CAPTURE_INVALID_ARGUMENT,
    GOLDEN_SCENE_CAPTURE_DESTINATION_EXISTS,
    GOLDEN_SCENE_CAPTURE_NO_VISIBLE_WINDOWS,
    GOLDEN_SCENE_CAPTURE_SCREEN_FAILED,
    GOLDEN_SCENE_CAPTURE_SAVE_FAILED
} GoldenSceneCaptureStatus;

/* Captures one screen-composited scene into memory without changing files. */
GoldenSceneCaptureStatus golden_capture_scene_image(
    HWND foreground, GoldenImage *image, RECT *captured_bounds);

/* Saves one screen-composited crop containing the foreground application's
   root window and all of its visible owned top-level windows. */
GoldenSceneCaptureStatus golden_capture_scene(
    IWICImagingFactory *factory, HWND foreground, const wchar_t *png_path,
    RECT *captured_bounds);

#endif

#ifndef GOLDENS_WINDOW_CAPTURE_H
#define GOLDENS_WINDOW_CAPTURE_H

#include "image_io.h"

typedef BOOL (*GoldenWindowRenderer)(HWND window, HDC destination,
                                     const RECT *bounds, BYTE *pixels,
                                     int width, int height, void *context);

/* Maps the visible DWM frame into a bitmap rendered for GetWindowRect. */
BOOL golden_window_capture_source_rect(const RECT *window_bounds,
                                       const RECT *visible_bounds,
                                       RECT *source);

/* Captures a top-level window without changing its placement or activation. */
BOOL golden_capture_window(HWND window, GoldenImage *image);
BOOL golden_capture_window_with_renderer(HWND window, GoldenImage *image,
                                         GoldenWindowRenderer renderer,
                                         void *context);

#endif

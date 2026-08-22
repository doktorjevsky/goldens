#ifndef GOLDENS_PREVIEW_CAPTURE_H
#define GOLDENS_PREVIEW_CAPTURE_H

#include "image_io.h"

typedef BOOL (*GoldenPreviewRenderer)(HWND window, HDC destination,
                                      const RECT *bounds, BYTE *pixels,
                                      int width, int height, void *context);

/* Captures a top-level window without changing its placement or activation. */
BOOL golden_capture_window_preview(HWND window, GoldenImage *image);

/* Renderer injection keeps the allocation/copy path testable without a desktop session. */
BOOL golden_capture_window_preview_with_renderer(HWND window, GoldenImage *image,
                                                 GoldenPreviewRenderer renderer,
                                                 void *context);

#endif

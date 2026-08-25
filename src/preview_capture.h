#ifndef GOLDENS_PREVIEW_CAPTURE_H
#define GOLDENS_PREVIEW_CAPTURE_H

#include "image_io.h"

typedef BOOL (*GoldenPreviewRenderer)(HWND window, HDC destination,
                                      const RECT *bounds, BYTE *pixels,
                                      int width, int height, void *context);

/* Maps the visible DWM bounds into a bitmap rendered for GetWindowRect. */
BOOL golden_preview_source_rect(const RECT *window_bounds,
                                const RECT *visible_bounds, RECT *source);

/* Captures a top-level window without changing its placement or activation. */
BOOL golden_capture_window_preview(HWND window, GoldenImage *image);

/* The renderer receives the full GetWindowRect target before visible-frame cropping. */
BOOL golden_capture_window_preview_with_renderer(HWND window, GoldenImage *image,
                                                 GoldenPreviewRenderer renderer,
                                                 void *context);

#endif

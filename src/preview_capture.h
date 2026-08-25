#ifndef GOLDENS_PREVIEW_CAPTURE_H
#define GOLDENS_PREVIEW_CAPTURE_H

#include "image_io.h"

typedef BOOL (*GoldenPreviewRenderer)(HWND window, HDC destination,
                                      const RECT *bounds, BYTE *pixels,
                                      int width, int height, void *context);

typedef struct {
    HDC memory;
    HBITMAP bitmap;
    HGDIOBJ original_bitmap;
    BYTE *pixels;
    UINT width;
    UINT height;
    UINT stride;
} GoldenPreviewSurface;

BOOL golden_preview_surface_capture(GoldenPreviewSurface *surface, HWND window);
BOOL golden_preview_surface_capture_with_renderer(
    GoldenPreviewSurface *surface, HWND window,
    GoldenPreviewRenderer renderer, void *context);
GoldenImage golden_preview_surface_image(const GoldenPreviewSurface *surface);
void golden_preview_surface_release(GoldenPreviewSurface *surface);

/* Captures a top-level window without changing its placement or activation. */
BOOL golden_capture_window_preview(HWND window, GoldenImage *image);

/* Compatibility wrapper that returns an independently owned pixel copy. */
BOOL golden_capture_window_preview_with_renderer(HWND window, GoldenImage *image,
                                                 GoldenPreviewRenderer renderer,
                                                 void *context);

#endif

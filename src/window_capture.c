#include "window_capture.h"

#include <dwmapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

static BOOL pixels_have_content(const BYTE *pixels, int width, int height) {
    size_t count = (size_t)width * (size_t)height;
    size_t step = max((size_t)1, count / 2048);
    for (size_t i = 0; i < count; i += step) {
        const BYTE *pixel = pixels + i * 4;
        if (pixel[0] || pixel[1] || pixel[2]) return TRUE;
    }
    return FALSE;
}

BOOL golden_window_capture_source_rect(const RECT *window_bounds,
                                       const RECT *visible_bounds,
                                       RECT *source) {
    if (!window_bounds || !visible_bounds || !source ||
        window_bounds->right <= window_bounds->left ||
        window_bounds->bottom <= window_bounds->top ||
        visible_bounds->right <= visible_bounds->left ||
        visible_bounds->bottom <= visible_bounds->top ||
        visible_bounds->left < window_bounds->left ||
        visible_bounds->top < window_bounds->top ||
        visible_bounds->right > window_bounds->right ||
        visible_bounds->bottom > window_bounds->bottom)
        return FALSE;
    source->left = visible_bounds->left - window_bounds->left;
    source->top = visible_bounds->top - window_bounds->top;
    source->right = visible_bounds->right - window_bounds->left;
    source->bottom = visible_bounds->bottom - window_bounds->top;
    return TRUE;
}

static BOOL render_window(HWND window, HDC destination, const RECT *bounds,
                          BYTE *pixels, int width, int height, void *context) {
    UNREFERENCED_PARAMETER(context);
    PatBlt(destination, 0, 0, width, height, BLACKNESS);
    BOOL rendered = PrintWindow(window, destination, PW_RENDERFULLCONTENT);
    if (!rendered || !pixels_have_content(pixels, width, height)) {
        PatBlt(destination, 0, 0, width, height, BLACKNESS);
        rendered = PrintWindow(window, destination, 0);
    }
    if ((!rendered || !pixels_have_content(pixels, width, height)) &&
        !IsIconic(window)) {
        HDC screen = GetDC(NULL);
        if (screen) {
            rendered = BitBlt(destination, 0, 0, width, height, screen,
                              bounds->left, bounds->top,
                              SRCCOPY | CAPTUREBLT);
            ReleaseDC(NULL, screen);
        } else rendered = FALSE;
    }
    return rendered;
}

static BOOL capture_geometry(HWND window, RECT *window_bounds,
                             RECT *source, int *width, int *height) {
    if (!window || !IsWindow(window)) return FALSE;
    RECT visible_bounds = {0};
    BOOL has_dwm_bounds = SUCCEEDED(DwmGetWindowAttribute(
        window, DWMWA_EXTENDED_FRAME_BOUNDS, &visible_bounds,
        sizeof(visible_bounds))) && visible_bounds.right > visible_bounds.left &&
        visible_bounds.bottom > visible_bounds.top;
    BOOL iconic = IsIconic(window);
    if (iconic && has_dwm_bounds) {
        *window_bounds = visible_bounds;
    } else if (!GetWindowRect(window, window_bounds)) {
        return FALSE;
    }
    if (!has_dwm_bounds || iconic) visible_bounds = *window_bounds;
    if (!golden_window_capture_source_rect(window_bounds, &visible_bounds,
                                           source)) return FALSE;
    *width = window_bounds->right - window_bounds->left;
    *height = window_bounds->bottom - window_bounds->top;
    return *width > 0 && *height > 0 && *width <= 32768 && *height <= 32768 &&
           (size_t)*width * 4u <= SIZE_MAX / (size_t)*height;
}

BOOL golden_capture_window_with_renderer(HWND window, GoldenImage *image,
                                         GoldenWindowRenderer renderer,
                                         void *context) {
    if (!image || !renderer) return FALSE;
    RECT window_bounds = {0}, source = {0};
    int render_width = 0, render_height = 0;
    if (!capture_geometry(window, &window_bounds, &source,
                          &render_width, &render_height)) return FALSE;

    HDC screen = GetDC(NULL);
    HDC memory = screen ? CreateCompatibleDC(screen) : NULL;
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = render_width;
    info.bmiHeader.biHeight = -render_height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    BYTE *pixels = NULL;
    HBITMAP bitmap = screen ? CreateDIBSection(
        screen, &info, DIB_RGB_COLORS, (void **)&pixels, NULL, 0) : NULL;
    HGDIOBJ previous = memory && bitmap ? SelectObject(memory, bitmap) : NULL;
    BOOL captured = previous && previous != HGDI_ERROR && pixels &&
        renderer(window, memory, &window_bounds, pixels,
                 render_width, render_height, context);

    GoldenImage result = {0};
    if (captured) {
        golden_bgra_force_opaque(pixels, (UINT)render_width,
                                 (UINT)render_height,
                                 (UINT)render_width * 4u);
        result.width = (UINT)(source.right - source.left);
        result.height = (UINT)(source.bottom - source.top);
        result.stride = result.width * 4u;
        size_t bytes = (size_t)result.stride * result.height;
        result.pixels = (BYTE *)malloc(bytes);
        if (result.pixels) {
            size_t source_stride = (size_t)render_width * 4u;
            const BYTE *first = pixels + (size_t)source.top * source_stride +
                                (size_t)source.left * 4u;
            for (UINT y = 0; y < result.height; ++y)
                memcpy(result.pixels + (size_t)y * result.stride,
                       first + (size_t)y * source_stride, result.stride);
        } else captured = FALSE;
    }

    if (previous && previous != HGDI_ERROR) SelectObject(memory, previous);
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(NULL, screen);
    if (!captured) {
        golden_image_free(&result);
        return FALSE;
    }
    golden_image_free(image);
    *image = result;
    return TRUE;
}

BOOL golden_capture_window(HWND window, GoldenImage *image) {
    return golden_capture_window_with_renderer(
        window, image, render_window, NULL);
}

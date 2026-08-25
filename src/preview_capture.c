#include "preview_capture.h"

#include <dwmapi.h>
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

BOOL golden_preview_source_rect(const RECT *window_bounds,
                                const RECT *visible_bounds, RECT *source) {
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
    if ((!rendered || !pixels_have_content(pixels, width, height)) && !IsIconic(window)) {
        HDC screen = GetDC(NULL);
        if (screen) {
            rendered = BitBlt(destination, 0, 0, width, height, screen,
                              bounds->left, bounds->top, SRCCOPY | CAPTUREBLT);
            ReleaseDC(NULL, screen);
        } else rendered = FALSE;
    }
    return rendered;
}

static BOOL capture_window_preview(HWND window, const RECT *window_bounds,
                                   const RECT *visible_bounds, GoldenImage *image,
                                   GoldenPreviewRenderer renderer, void *context) {
    if (!image) return FALSE;
    ZeroMemory(image, sizeof(*image));
    if (!window || !IsWindow(window) || !window_bounds || !visible_bounds || !renderer)
        return FALSE;

    RECT source = {0};
    if (!golden_preview_source_rect(window_bounds, visible_bounds, &source)) return FALSE;
    int width = window_bounds->right - window_bounds->left;
    int height = window_bounds->bottom - window_bounds->top;
    int output_width = source.right - source.left;
    int output_height = source.bottom - source.top;
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768) return FALSE;

    HDC screen = GetDC(NULL);
    HDC memory = screen ? CreateCompatibleDC(screen) : NULL;
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    BYTE *dib_pixels = NULL;
    HBITMAP bitmap = screen ? CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                                (void **)&dib_pixels, NULL, 0) : NULL;
    BOOL rendered = FALSE;
    if (memory && bitmap && dib_pixels) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        rendered = renderer(window, memory, window_bounds, dib_pixels, width, height, context);
        if (rendered) {
            size_t stride = (size_t)output_width * 4;
            size_t bytes = stride * (size_t)output_height;
            image->pixels = (BYTE *)malloc(bytes);
            if (image->pixels) {
                size_t render_stride = (size_t)width * 4;
                const BYTE *row = dib_pixels + (size_t)source.top * render_stride +
                                  (size_t)source.left * 4;
                for (int y = 0; y < output_height; ++y)
                    memcpy(image->pixels + (size_t)y * stride,
                           row + (size_t)y * render_stride, stride);
                image->width = (UINT)output_width;
                image->height = (UINT)output_height;
                image->stride = (UINT)stride;
            } else rendered = FALSE;
        }
        SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(NULL, screen);
    if (!rendered) golden_image_free(image);
    return rendered;
}

static BOOL preview_bounds(HWND window, RECT *window_bounds, RECT *visible_bounds) {
    if (!GetWindowRect(window, window_bounds)) return FALSE;
    *visible_bounds = *window_bounds;
    RECT dwm_bounds = {0}, source = {0};
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &dwm_bounds, sizeof(dwm_bounds))) &&
        golden_preview_source_rect(window_bounds, &dwm_bounds, &source))
        *visible_bounds = dwm_bounds;
    return TRUE;
}

BOOL golden_capture_window_preview_with_renderer(HWND window, GoldenImage *image,
                                                 GoldenPreviewRenderer renderer,
                                                 void *context) {
    if (!image) return FALSE;
    ZeroMemory(image, sizeof(*image));
    if (!window || !IsWindow(window) || !renderer) return FALSE;
    RECT window_bounds = {0}, visible_bounds = {0};
    if (!preview_bounds(window, &window_bounds, &visible_bounds)) return FALSE;
    return capture_window_preview(window, &window_bounds, &visible_bounds,
                                  image, renderer, context);
}

BOOL golden_capture_window_preview(HWND window, GoldenImage *image) {
    return golden_capture_window_preview_with_renderer(window, image, render_window, NULL);
}

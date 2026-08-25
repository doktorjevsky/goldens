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

static BOOL window_bounds(HWND window, RECT *bounds, int *width, int *height) {
    if (!window || !IsWindow(window)) return FALSE;
    RECT window_rectangle = {0};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                     &window_rectangle, sizeof(window_rectangle))) &&
        !GetWindowRect(window, &window_rectangle)) return FALSE;
    *width = window_rectangle.right - window_rectangle.left;
    *height = window_rectangle.bottom - window_rectangle.top;
    if (*width <= 0 || *height <= 0 || *width > 32768 || *height > 32768)
        return FALSE;
    *bounds = window_rectangle;
    return TRUE;
}

static BOOL prepare_surface(GoldenPreviewSurface *surface, int width, int height) {
    if (surface->memory && surface->bitmap && surface->pixels &&
        surface->width == (UINT)width && surface->height == (UINT)height)
        return TRUE;
    HDC screen = GetDC(NULL);
    HDC memory = surface->memory ? surface->memory :
                 screen ? CreateCompatibleDC(screen) : NULL;
    if (!screen || !memory) {
        if (screen) ReleaseDC(NULL, screen);
        return FALSE;
    }
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    BYTE *dib_pixels = NULL;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                      (void **)&dib_pixels, NULL, 0);
    if (!bitmap || !dib_pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (!surface->memory) DeleteDC(memory);
        ReleaseDC(NULL, screen);
        return FALSE;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    if (!previous || previous == HGDI_ERROR) {
        DeleteObject(bitmap);
        if (!surface->memory) DeleteDC(memory);
        ReleaseDC(NULL, screen);
        return FALSE;
    }
    if (!surface->memory) surface->original_bitmap = previous;
    else DeleteObject(surface->bitmap);
    surface->memory = memory;
    surface->bitmap = bitmap;
    surface->pixels = dib_pixels;
    surface->width = (UINT)width;
    surface->height = (UINT)height;
    surface->stride = (UINT)width * 4;
    ReleaseDC(NULL, screen);
    return TRUE;
}

BOOL golden_preview_surface_capture_with_renderer(
    GoldenPreviewSurface *surface, HWND window,
    GoldenPreviewRenderer renderer, void *context) {
    if (!surface || !renderer) return FALSE;
    RECT bounds;
    int width, height;
    if (!window_bounds(window, &bounds, &width, &height) ||
        !prepare_surface(surface, width, height)) return FALSE;
    return renderer(window, surface->memory, &bounds, surface->pixels,
                    width, height, context);
}

BOOL golden_preview_surface_capture(GoldenPreviewSurface *surface, HWND window) {
    return golden_preview_surface_capture_with_renderer(
        surface, window, render_window, NULL);
}

GoldenImage golden_preview_surface_image(const GoldenPreviewSurface *surface) {
    GoldenImage image = {0};
    if (surface && surface->pixels) {
        image.pixels = surface->pixels;
        image.width = surface->width;
        image.height = surface->height;
        image.stride = surface->stride;
    }
    return image;
}

void golden_preview_surface_release(GoldenPreviewSurface *surface) {
    if (!surface) return;
    if (surface->memory && surface->original_bitmap)
        SelectObject(surface->memory, surface->original_bitmap);
    if (surface->bitmap) DeleteObject(surface->bitmap);
    if (surface->memory) DeleteDC(surface->memory);
    *surface = (GoldenPreviewSurface){0};
}

BOOL golden_capture_window_preview_with_renderer(HWND window, GoldenImage *image,
                                                 GoldenPreviewRenderer renderer,
                                                 void *context) {
    if (!image) return FALSE;
    GoldenPreviewSurface surface = {0};
    BOOL rendered = golden_preview_surface_capture_with_renderer(
        &surface, window, renderer, context);
    GoldenImage captured = golden_preview_surface_image(&surface);
    if (rendered) {
        size_t bytes = (size_t)captured.stride * captured.height;
        captured.pixels = (BYTE *)malloc(bytes);
        if (captured.pixels) memcpy(captured.pixels, surface.pixels, bytes);
        else rendered = FALSE;
    }
    golden_preview_surface_release(&surface);
    if (!rendered) {
        golden_image_free(&captured);
        return FALSE;
    }
    golden_image_free(image);
    *image = captured;
    return TRUE;
}

BOOL golden_capture_window_preview(HWND window, GoldenImage *image) {
    return golden_capture_window_preview_with_renderer(window, image, render_window, NULL);
}

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

BOOL golden_capture_window_preview_with_renderer(HWND window, GoldenImage *image,
                                                 GoldenPreviewRenderer renderer,
                                                 void *context) {
    if (!image) return FALSE;
    ZeroMemory(image, sizeof(*image));
    if (!window || !IsWindow(window) || !renderer) return FALSE;

    RECT bounds = {0};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                     &bounds, sizeof(bounds))))
        GetWindowRect(window, &bounds);
    int width = bounds.right - bounds.left;
    int height = bounds.bottom - bounds.top;
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
        rendered = renderer(window, memory, &bounds, dib_pixels, width, height, context);
        if (rendered) {
            size_t bytes = (size_t)width * (size_t)height * 4;
            image->pixels = (BYTE *)malloc(bytes);
            if (image->pixels) {
                memcpy(image->pixels, dib_pixels, bytes);
                image->width = (UINT)width;
                image->height = (UINT)height;
                image->stride = (UINT)width * 4;
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

BOOL golden_capture_window_preview(HWND window, GoldenImage *image) {
    return golden_capture_window_preview_with_renderer(window, image, render_window, NULL);
}

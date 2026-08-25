#include "editor_render.h"

#include <limits.h>
#include <wingdi.h>

#define GOLDEN_IMAGE_CACHE_MAX_PIXELS (32u * 1024u * 1024u)

BOOL golden_back_buffer_ensure(GoldenBackBuffer *buffer, HDC reference,
                               int width, int height) {
    if (!buffer || !reference || width <= 0 || height <= 0) return FALSE;
    if (buffer->dc && buffer->bitmap &&
        buffer->width >= width && buffer->height >= height) return TRUE;

    int new_width = max(width, buffer->width);
    int new_height = max(height, buffer->height);
    HDC dc = buffer->dc ? buffer->dc : CreateCompatibleDC(reference);
    if (!dc) return FALSE;
    HBITMAP bitmap = CreateCompatibleBitmap(reference, new_width, new_height);
    if (!bitmap) {
        if (!buffer->dc) DeleteDC(dc);
        return FALSE;
    }

    HGDIOBJ previous = SelectObject(dc, bitmap);
    if (!previous || previous == HGDI_ERROR) {
        DeleteObject(bitmap);
        if (!buffer->dc) DeleteDC(dc);
        return FALSE;
    }
    if (!buffer->dc) buffer->original_bitmap = previous;
    else DeleteObject(buffer->bitmap);
    buffer->dc = dc;
    buffer->bitmap = bitmap;
    buffer->width = new_width;
    buffer->height = new_height;
    return TRUE;
}

void golden_back_buffer_release(GoldenBackBuffer *buffer) {
    if (!buffer) return;
    if (buffer->dc && buffer->original_bitmap)
        SelectObject(buffer->dc, buffer->original_bitmap);
    if (buffer->bitmap) DeleteObject(buffer->bitmap);
    if (buffer->dc) DeleteDC(buffer->dc);
    *buffer = (GoldenBackBuffer){0};
}

int golden_draw_bgra_image(HDC dc, const BYTE *pixels, UINT width, UINT height,
                           const RECT *destination, double scale) {
    if (!dc || !pixels || !width || !height || width > INT_MAX ||
        height > INT_MAX || !destination) return 0;
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = (LONG)width;
    info.bmiHeader.biHeight = -(LONG)height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (scale == 1.0) SetStretchBltMode(dc, COLORONCOLOR);
    else {
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
    }
    return StretchDIBits(dc, destination->left, destination->top,
                         destination->right - destination->left,
                         destination->bottom - destination->top,
                         0, 0, (int)width, (int)height, pixels, &info,
                         DIB_RGB_COLORS, SRCCOPY);
}

BOOL golden_draw_cached_bgra_image(GoldenImageCache *cache, HDC dc,
                                   const BYTE *pixels, UINT width, UINT height,
                                   const RECT *destination, double scale,
                                   uint64_t revision) {
    if (!cache || !dc || !pixels || !width || !height || !destination)
        return FALSE;
    int destination_width = destination->right - destination->left;
    int destination_height = destination->bottom - destination->top;
    if (destination_width <= 0 || destination_height <= 0 ||
        (size_t)destination_width * (size_t)destination_height >
            GOLDEN_IMAGE_CACHE_MAX_PIXELS) return FALSE;

    BOOL exact_scale = scale == 1.0;
    BOOL changed = !cache->valid || cache->revision != revision ||
                   cache->source_width != width || cache->source_height != height ||
                   cache->width != destination_width ||
                   cache->height != destination_height ||
                   cache->exact_scale != exact_scale;
    if (changed) {
        if (!golden_back_buffer_ensure(&cache->surface, dc,
                                       destination_width, destination_height))
            return FALSE;
        RECT cached_destination = {0, 0, destination_width, destination_height};
        if (!golden_draw_bgra_image(cache->surface.dc, pixels, width, height,
                                    &cached_destination, scale)) return FALSE;
        cache->revision = revision;
        cache->source_width = width;
        cache->source_height = height;
        cache->width = destination_width;
        cache->height = destination_height;
        cache->exact_scale = exact_scale;
        cache->valid = TRUE;
    }
    return BitBlt(dc, destination->left, destination->top,
                  destination_width, destination_height,
                  cache->surface.dc, 0, 0, SRCCOPY);
}

void golden_image_cache_release(GoldenImageCache *cache) {
    if (!cache) return;
    golden_back_buffer_release(&cache->surface);
    *cache = (GoldenImageCache){0};
}

void golden_draw_boundary(HDC dc, const RECT *boundary, COLORREF color,
                          int thickness, int pen_style) {
    if (!dc || !boundary) return;
    HPEN pen = CreatePen(pen_style, thickness, color);
    if (!pen) return;
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, boundary->left, boundary->top, boundary->right, boundary->bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void golden_fill_tinted_rect(HDC dc, const RECT *boundary, COLORREF color,
                             BYTE opacity) {
    int width = boundary ? boundary->right - boundary->left : 0;
    int height = boundary ? boundary->bottom - boundary->top : 0;
    if (!dc || width <= 0 || height <= 0 || !opacity) return;
    HDC source = CreateCompatibleDC(dc);
    HBITMAP bitmap = source ? CreateCompatibleBitmap(dc, width, height) : NULL;
    if (bitmap) {
        HGDIOBJ previous = SelectObject(source, bitmap);
        RECT fill = {0, 0, width, height};
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(source, &fill, brush);
        DeleteObject(brush);
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, opacity, 0};
        AlphaBlend(dc, boundary->left, boundary->top, width, height,
                   source, 0, 0, width, height, blend);
        SelectObject(source, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (source) DeleteDC(source);
}

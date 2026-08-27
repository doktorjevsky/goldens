#ifndef GOLDENS_EDITOR_RENDER_H
#define GOLDENS_EDITOR_RENDER_H

#include <windows.h>
#include <stdint.h>

typedef struct {
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ original_bitmap;
    int width;
    int height;
} GoldenBackBuffer;

typedef struct {
    GoldenBackBuffer surface;
    uint64_t revision;
    UINT source_width;
    UINT source_height;
    int width;
    int height;
    BOOL exact_scale;
    BOOL valid;
} GoldenImageCache;

BOOL golden_back_buffer_ensure(GoldenBackBuffer *buffer, HDC reference,
                               int width, int height);
void golden_back_buffer_release(GoldenBackBuffer *buffer);

BOOL golden_draw_cached_bgra_image(GoldenImageCache *cache, HDC dc,
                                   const BYTE *pixels, UINT width, UINT height,
                                   const RECT *destination, double scale,
                                   uint64_t revision);
void golden_image_cache_release(GoldenImageCache *cache);

int golden_draw_bgra_image(HDC dc, const BYTE *pixels, UINT width, UINT height,
                           const RECT *destination, double scale);
void golden_draw_boundary(HDC dc, const RECT *boundary, COLORREF color,
                          int thickness, int pen_style);
void golden_draw_click_mark(HDC dc, POINT center);
void golden_fill_tinted_rect(HDC dc, const RECT *boundary, COLORREF color,
                             BYTE opacity);

#endif

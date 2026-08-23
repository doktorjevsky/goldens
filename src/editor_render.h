#ifndef GOLDENS_EDITOR_RENDER_H
#define GOLDENS_EDITOR_RENDER_H

#include <windows.h>

int golden_draw_bgra_image(HDC dc, const BYTE *pixels, UINT width, UINT height,
                           const RECT *destination, double scale);
void golden_draw_boundary(HDC dc, const RECT *boundary, COLORREF color,
                          int thickness, int pen_style);
void golden_fill_tinted_rect(HDC dc, const RECT *boundary, COLORREF color,
                             BYTE opacity);

#endif

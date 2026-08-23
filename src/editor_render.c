#include "editor_render.h"

#include <wingdi.h>

int golden_draw_bgra_image(HDC dc, const BYTE *pixels, UINT width, UINT height,
                           const RECT *destination, double scale) {
    if (!dc || !pixels || !width || !height || !destination) return 0;
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
                         0, 0, width, height, pixels, &info,
                         DIB_RGB_COLORS, SRCCOPY);
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

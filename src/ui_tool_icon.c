#include "ui_tool_icon.h"

#include <stdint.h>

#include "ui_tool_icon_masks.h"

static int source_coordinate(int destination_coordinate, int source_size,
                             int destination_size, int *fraction,
                             int *denominator) {
    int scale = 2 * destination_size;
    int numerator = (2 * destination_coordinate + 1) * source_size -
                    destination_size;
    int coordinate = numerator >= 0 ? numerator / scale : -1;
    *fraction = numerator - coordinate * scale;
    *denominator = scale;
    return coordinate;
}

static BYTE mask_pixel(const unsigned char *mask, int size, int x, int y) {
    x = max(0, min(size - 1, x));
    y = max(0, min(size - 1, y));
    return mask[y * size + x];
}

static BYTE scaled_mask_pixel(const unsigned char *mask, int source_size,
                              int destination_size, int x, int y) {
    if (source_size == destination_size)
        return mask[y * source_size + x];

    int x_fraction, x_denominator, y_fraction, y_denominator;
    int source_x = source_coordinate(x, source_size, destination_size,
                                     &x_fraction, &x_denominator);
    int source_y = source_coordinate(y, source_size, destination_size,
                                     &y_fraction, &y_denominator);
    int top = (mask_pixel(mask, source_size, source_x, source_y) *
                   (x_denominator - x_fraction) +
               mask_pixel(mask, source_size, source_x + 1, source_y) *
                   x_fraction + x_denominator / 2) / x_denominator;
    int bottom = (mask_pixel(mask, source_size, source_x, source_y + 1) *
                      (x_denominator - x_fraction) +
                  mask_pixel(mask, source_size, source_x + 1, source_y + 1) *
                      x_fraction + x_denominator / 2) / x_denominator;
    return (BYTE)((top * (y_denominator - y_fraction) +
                   bottom * y_fraction + y_denominator / 2) / y_denominator);
}

void golden_draw_tool_icon(HDC dc, GoldenToolIcon icon, const RECT *bounds,
                           COLORREF color, UINT dpi) {
    if (!dc || !bounds || icon < GOLDEN_TOOL_ICON_SELECT ||
        icon > GOLDEN_TOOL_ICON_CLEAR_CLICK) return;
    int width = bounds->right - bounds->left;
    int height = bounds->bottom - bounds->top;
    if (width <= 0 || height <= 0) return;

    int size = min(MulDiv(20, dpi ? (int)dpi : 96, 96),
                   min(width - 4, height - 4));
    if (size < 8) return;
    int x = bounds->left + (width - size) / 2;
    int y = bounds->top + (height - size) / 2;
    int source_size = size <= 20 ? 20 : 40;
    const unsigned char *mask = source_size == 20 ?
        golden_tool_icon_masks_20[icon] : golden_tool_icon_masks_40[icon];

    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    DWORD *pixels = NULL;
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
                                      (void **)&pixels, NULL, 0);
    if (!memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        return;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);

    BYTE red = GetRValue(color), green = GetGValue(color), blue = GetBValue(color);
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            BYTE alpha = scaled_mask_pixel(mask, source_size, size, px, py);
            pixels[py * size + px] = ((DWORD)alpha << 24) |
                ((DWORD)(red * alpha / 255) << 16) |
                ((DWORD)(green * alpha / 255) << 8) |
                (DWORD)(blue * alpha / 255);
        }
    }

    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, x, y, size, size, memory, 0, 0, size, size, blend);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

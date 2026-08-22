#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../src/editor_render.h"

int main(void) {
    BYTE source[4 * 4 * 4];
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
        int offset = (y * 4 + x) * 4;
        source[offset + 0] = (BYTE)(x * 47 + y);
        source[offset + 1] = (BYTE)(y * 53 + x);
        source[offset + 2] = (BYTE)(x * 17 + y * 23);
        source[offset + 3] = 0;
    }
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 4;
    info.bmiHeader.biHeight = -4;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(NULL);
    HDC destination_dc = CreateCompatibleDC(screen);
    BYTE *destination = NULL;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                      (void **)&destination, NULL, 0);
    HGDIOBJ old = SelectObject(destination_dc, bitmap);
    RECT destination_rect = {0, 0, 4, 4};
    int lines = golden_draw_bgra_image(destination_dc, source, 4, 4,
                                       &destination_rect, 1.0);
    int failed = lines != 4;
    for (int i = 0; !failed && i < 16; ++i)
        if (memcmp(source + i * 4, destination + i * 4, 3)) failed = 1;

    RECT outline = {1, 1, 4, 4};
    COLORREF before_tint = GetPixel(destination_dc, 2, 2);
    golden_fill_tinted_rect(destination_dc, &outline, RGB(255, 150, 0), 112);
    COLORREF after_tint = GetPixel(destination_dc, 2, 2);
    if (after_tint == before_tint) failed = 1;
    golden_draw_boundary(destination_dc, &outline, RGB(255, 0, 0), 1, PS_SOLID);
    COLORREF edge = GetPixel(destination_dc, 1, 1);
    COLORREF interior = GetPixel(destination_dc, 2, 2);
    if (edge != RGB(255, 0, 0)) failed = 1;
    if (interior == RGB(255, 0, 0)) failed = 1;
    SelectObject(destination_dc, old);
    DeleteObject(bitmap);
    DeleteDC(destination_dc);
    ReleaseDC(NULL, screen);
    if (failed) return 1;
    puts("All Goldens render fidelity tests passed.");
    return 0;
}

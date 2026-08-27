#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../src/ui_tool_icon.h"

static int count_changed_pixels(const DWORD *pixels, int width, int height) {
    int changed = 0;
    for (int i = 0; i < width * height; ++i)
        if ((pixels[i] & 0x00ffffff) != 0x00ffffff) changed++;
    return changed;
}

static int changed_outside(const DWORD *pixels, int width, int height,
                           const RECT *bounds) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (x >= bounds->left && x < bounds->right &&
                y >= bounds->top && y < bounds->bottom) continue;
            if ((pixels[y * width + x] & 0x00ffffff) != 0x00ffffff) return 1;
        }
    }
    return 0;
}

int main(void) {
    const int width = 96, height = 96;
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    DWORD *pixels = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
                                      (void **)&pixels, NULL, 0);
    if (!dc || !bitmap || !pixels) return 1;
    HGDIOBJ previous = SelectObject(dc, bitmap);
    RECT bounds = {16, 16, 80, 80};
    int failed = 0;

    for (int dpi_index = 0; dpi_index < 2; ++dpi_index) {
        UINT dpi = dpi_index ? 192 : 96;
        for (int icon = GOLDEN_TOOL_ICON_SELECT;
             icon <= GOLDEN_TOOL_ICON_CLEAR_CLICK; ++icon) {
            for (int i = 0; i < width * height; ++i) pixels[i] = 0x00ffffff;
            golden_draw_tool_icon(dc, (GoldenToolIcon)icon, &bounds,
                                  RGB(12, 34, 56), dpi);
            if (count_changed_pixels(pixels, width, height) < 12 ||
                changed_outside(pixels, width, height, &bounds)) failed = 1;
        }
    }

    for (int i = 0; i < width * height; ++i) pixels[i] = 0x00ffffff;
    golden_draw_tool_icon(dc, GOLDEN_TOOL_ICON_CLICK, &bounds,
                          RGB(12, 34, 56), 96);
    DWORD click_pixels[96 * 96];
    memcpy(click_pixels, pixels, sizeof(click_pixels));
    for (int i = 0; i < width * height; ++i) pixels[i] = 0x00ffffff;
    golden_draw_tool_icon(dc, GOLDEN_TOOL_ICON_CLEAR_CLICK, &bounds,
                          RGB(12, 34, 56), 96);
    if (!memcmp(click_pixels, pixels, sizeof(click_pixels))) failed = 1;

    for (int i = 0; i < width * height; ++i) pixels[i] = 0x00ffffff;
    RECT too_small = {20, 20, 24, 24};
    golden_draw_tool_icon(dc, GOLDEN_TOOL_ICON_SELECT, &too_small,
                          RGB(12, 34, 56), 96);
    if (count_changed_pixels(pixels, width, height) != 0) failed = 1;

    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    if (failed) return 1;
    puts("All Goldens tool icon rendering tests passed.");
    return 0;
}

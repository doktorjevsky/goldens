#ifndef GOLDENS_UI_TOOL_ICON_H
#define GOLDENS_UI_TOOL_ICON_H

#include <windows.h>

typedef enum {
    GOLDEN_TOOL_ICON_SELECT,
    GOLDEN_TOOL_ICON_RECTANGLE,
    GOLDEN_TOOL_ICON_CLICK,
    GOLDEN_TOOL_ICON_CLEAR_CLICK
} GoldenToolIcon;

void golden_draw_tool_icon(HDC dc, GoldenToolIcon icon, const RECT *bounds,
                           COLORREF color, UINT dpi);

#endif

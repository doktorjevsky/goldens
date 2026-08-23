#ifndef GOLDENS_UI_TOOLTIP_H
#define GOLDENS_UI_TOOLTIP_H

#include <windows.h>
#include <commctrl.h>

typedef enum {
    GOLDEN_TOOLTIP_HOVER_NONE,
    GOLDEN_TOOLTIP_HOVER_HIDE,
    GOLDEN_TOOLTIP_HOVER_SCHEDULE
} GoldenTooltipHoverAction;

BOOL golden_tooltip_register_tracking(HWND tooltip, HWND tool_window,
                                      HINSTANCE instance, wchar_t *text,
                                      TOOLINFOW *tool);
void golden_tooltip_show(HWND tooltip, TOOLINFOW *tool, wchar_t *text,
                         POINT screen_position);
void golden_tooltip_hide(HWND tooltip, TOOLINFOW *tool);
GoldenTooltipHoverAction golden_tooltip_hover_action(int hit,
    int pending, int visible);

#endif

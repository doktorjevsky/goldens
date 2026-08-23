#include "ui_tooltip.h"

BOOL golden_tooltip_register_tracking(HWND tooltip, HWND tool_window,
                                      HINSTANCE instance, wchar_t *text,
                                      TOOLINFOW *tool) {
    if (!tooltip || !tool_window || !tool) return FALSE;
    ZeroMemory(tool, sizeof(*tool));
    tool->cbSize = TTTOOLINFO_V1_SIZE;
    tool->uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    tool->hwnd = tool_window;
    tool->uId = (UINT_PTR)tool_window;
    tool->hinst = instance;
    tool->lpszText = text;
    GetClientRect(tool_window, &tool->rect);
    return (BOOL)SendMessageW(tooltip, TTM_ADDTOOLW, 0, (LPARAM)tool);
}

void golden_tooltip_show(HWND tooltip, TOOLINFOW *tool, wchar_t *text,
                         POINT screen_position) {
    if (!tooltip || !tool) return;
    tool->lpszText = text;
    SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)tool);
    SendMessageW(tooltip, TTM_TRACKACTIVATE, TRUE, (LPARAM)tool);
    SendMessageW(tooltip, TTM_TRACKPOSITION, 0,
                 MAKELPARAM(screen_position.x, screen_position.y));
}

void golden_tooltip_hide(HWND tooltip, TOOLINFOW *tool) {
    if (tooltip && tool) {
        SendMessageW(tooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)tool);
        SendMessageW(tooltip, TTM_POP, 0, 0);
    }
}

GoldenTooltipHoverAction golden_tooltip_hover_action(int hit,
                                                       int pending,
                                                       int visible) {
    if (hit >= 0) {
        if (hit == pending || hit == visible) return GOLDEN_TOOLTIP_HOVER_NONE;
        return GOLDEN_TOOLTIP_HOVER_SCHEDULE;
    }
    return pending >= 0 || visible >= 0 ? GOLDEN_TOOLTIP_HOVER_HIDE :
                                         GOLDEN_TOOLTIP_HOVER_NONE;
}

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <wchar.h>

#include "../src/ui_tooltip.h"

int main(void) {
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND parent = CreateWindowExW(0, L"STATIC", L"Tooltip test",
        WS_OVERLAPPEDWINDOW, 100, 100, 400, 300,
        NULL, NULL, instance, NULL);
    HWND editor = CreateWindowExW(0, L"STATIC", NULL, WS_CHILD,
        0, 0, 300, 200, parent, NULL, instance, NULL);
    HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX | TTS_NOANIMATE | TTS_NOFADE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, parent, NULL, instance, NULL);
    if (!parent || !editor || !tooltip) return 1;

    wchar_t text[] = L"annotation_name";
    TOOLINFOW tool;
    int failed = !golden_tooltip_register_tracking(
        tooltip, editor, instance, text, &tool);
    if (!failed && golden_tooltip_hover_action(0, -1, -1) !=
            GOLDEN_TOOLTIP_HOVER_SCHEDULE) failed = 1;
    if (!failed && golden_tooltip_hover_action(0, 0, -1) !=
            GOLDEN_TOOLTIP_HOVER_NONE) failed = 1;
    if (!failed && golden_tooltip_hover_action(0, -1, 0) !=
            GOLDEN_TOOLTIP_HOVER_NONE) failed = 1;
    if (!failed && golden_tooltip_hover_action(-1, -1, 0) !=
            GOLDEN_TOOLTIP_HOVER_HIDE) failed = 1;
    if (!failed && golden_tooltip_hover_action(-1, 1, -1) !=
            GOLDEN_TOOLTIP_HOVER_HIDE) failed = 1;
    if (!failed && golden_tooltip_hover_action(1, -1, 0) !=
            GOLDEN_TOOLTIP_HOVER_SCHEDULE) failed = 1;
    if (!failed && tool.cbSize != TTTOOLINFO_V1_SIZE) failed = 1;
    if (!failed && SendMessageW(tooltip, TTM_GETTOOLCOUNT, 0, 0) != 1) failed = 1;

    POINT position = {180, 180};
    if (!failed) golden_tooltip_show(tooltip, &tool, text, position);
    wchar_t stored_text[128] = L"";
    TOOLINFOW query = tool;
    query.lpszText = stored_text;
    if (!failed) SendMessageW(tooltip, TTM_GETTEXTW, _countof(stored_text),
                              (LPARAM)&query);
    if (!failed && wcscmp(stored_text, text)) failed = 1;
    RECT rectangle = {0};
    if (!failed) GetWindowRect(tooltip, &rectangle);
    if (!failed && (rectangle.left != position.x || rectangle.top != position.y ||
        rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top)) failed = 1;
    golden_tooltip_hide(tooltip, &tool);

    DestroyWindow(tooltip);
    DestroyWindow(parent);
    if (failed) return 1;
    puts("All Goldens tooltip lifecycle tests passed.");
    return 0;
}

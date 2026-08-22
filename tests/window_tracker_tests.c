#include "../src/window_tracker.h"

#include <stdio.h>
#include <wchar.h>

static int find_window(const GoldenWindowInfo *items, int count, HWND hwnd) {
    for (int i = 0; i < count; ++i) if ((HWND)items[i].id == hwnd) return i;
    return -1;
}

static LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, message, wp, lp);
}

static void settle_windows(void) {
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Sleep(75);
}

int main(void) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW window_class = {0};
    window_class.lpfnWndProc = TestWindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = L"GoldensWindowTrackerTest";
    if (!RegisterClassW(&window_class)) return 1;
    HWND first = CreateWindowW(window_class.lpszClassName, L"Tracker first", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        0, 0, 200, 100, NULL, NULL, instance, NULL);
    HWND second = CreateWindowW(window_class.lpszClassName, L"Tracker second", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        0, 0, 200, 100, NULL, NULL, instance, NULL);
    if (!first || !second) return 2;
    ShowWindow(first, SW_SHOWNA);
    ShowWindow(second, SW_SHOWMINNOACTIVE);
    if (!(GetWindowLongPtrW(first, GWL_STYLE) & WS_VISIBLE)) return 7;
    if (!(GetWindowLongPtrW(second, GWL_STYLE) & WS_VISIBLE)) return 8;
    UpdateWindow(first);
    UpdateWindow(second);
    settle_windows();
    GoldenWindowInfo items[MAX_WINDOWS];
    int count = 0, first_index = -1, second_index = -1;
    for (int attempt = 0; attempt < 20 && (first_index < 0 || second_index < 0); ++attempt) {
        count = golden_collect_windows(NULL, items, MAX_WINDOWS);
        first_index = find_window(items, count, first);
        second_index = find_window(items, count, second);
        if (first_index < 0 || second_index < 0) settle_windows();
    }
    if (first_index < 0 || second_index < 0) return 3;
    if (wcscmp(items[second_index].title, L"Tracker second")) return 4;
    SetWindowTextW(second, L"Tracker renamed");
    settle_windows();
    second_index = -1;
    for (int attempt = 0; attempt < 20 && second_index < 0; ++attempt) {
        count = golden_collect_windows(NULL, items, MAX_WINDOWS);
        second_index = find_window(items, count, second);
        if (second_index >= 0 && !wcscmp(items[second_index].title, L"Tracker renamed")) break;
        second_index = -1;
        settle_windows();
    }
    if (second_index < 0 || wcscmp(items[second_index].title, L"Tracker renamed")) return 5;
    DestroyWindow(first);
    settle_windows();
    for (int attempt = 0; attempt < 20; ++attempt) {
        count = golden_collect_windows(NULL, items, MAX_WINDOWS);
        if (find_window(items, count, first) < 0 && find_window(items, count, second) >= 0) break;
        settle_windows();
    }
    if (find_window(items, count, first) >= 0 || find_window(items, count, second) < 0) return 6;
    DestroyWindow(second);
    puts("All Goldens window tracker tests passed.");
    return 0;
}

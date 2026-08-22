#include "window_tracker.h"

#include <stdlib.h>
#include <wchar.h>

typedef struct {
    HWND excluded;
    GoldenWindowInfo *items;
    int count;
    int capacity;
} WindowCollector;

static BOOL CALLBACK collect_window(HWND hwnd, LPARAM parameter) {
    WindowCollector *collector = (WindowCollector *)parameter;
    if (hwnd == collector->excluded || !(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) ||
        collector->count >= collector->capacity) return TRUE;
    wchar_t title[256];
    if (!GetWindowTextW(hwnd, title, 256) || !title[0]) return TRUE;
    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    wchar_t app[128] = L"Unknown application";
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process) {
        wchar_t path[1024];
        DWORD size = 1024;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) {
            const wchar_t *slash = wcsrchr(path, L'\\');
            wcsncpy(app, slash ? slash + 1 : path, 127);
            app[127] = 0;
        }
        CloseHandle(process);
    }
    GoldenWindowInfo *item = &collector->items[collector->count++];
    item->id = (UINT_PTR)hwnd;
    wcscpy(item->app, app);
    wcsncpy(item->title, title, 255);
    item->title[255] = 0;
    return TRUE;
}

static int compare_windows(const void *left, const void *right) {
    const GoldenWindowInfo *a = (const GoldenWindowInfo *)left;
    const GoldenWindowInfo *b = (const GoldenWindowInfo *)right;
    int app = _wcsicmp(a->app, b->app);
    if (app) return app;
    int title = _wcsicmp(a->title, b->title);
    if (title) return title;
    return a->id < b->id ? -1 : a->id > b->id;
}

int golden_collect_windows(HWND excluded, GoldenWindowInfo *items, int capacity) {
    if (!items || capacity <= 0) return 0;
    WindowCollector collector = {excluded, items, 0, capacity};
    EnumWindows(collect_window, (LPARAM)&collector);
    qsort(items, collector.count, sizeof(*items), compare_windows);
    return collector.count;
}

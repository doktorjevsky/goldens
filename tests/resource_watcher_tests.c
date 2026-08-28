#include <windows.h>

#include <stdio.h>

#include "../src/resource_watcher.h"

#define RESOURCE_CHANGED (WM_APP + 47)

static LRESULT CALLBACK test_window_proc(HWND window, UINT message,
                                         WPARAM wp, LPARAM lp) {
    return DefWindowProcW(window, message, wp, lp);
}

static int wait_for_change(DWORD timeout_ms) {
    DWORD start = GetTickCount();
    for (;;) {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == RESOURCE_CHANGED) return message.wParam == 0;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeout_ms) return 0;
        MsgWaitForMultipleObjects(0, NULL, FALSE, timeout_ms - elapsed,
                                  QS_ALLINPUT);
    }
}

static int drain_changes(void) {
    MSG message;
    int count = 0;
    while (PeekMessageW(&message, NULL, RESOURCE_CHANGED, RESOURCE_CHANGED,
                        PM_REMOVE))
        ++count;
    return count;
}

static int wait_for_new_generation(const GoldenResourceWatcher *watcher,
                                   LONG previous, DWORD timeout_ms) {
    DWORD start = GetTickCount();
    while (golden_resource_watcher_generation(watcher) == previous) {
        if (GetTickCount() - start >= timeout_ms) return 0;
        Sleep(1);
    }
    return 1;
}

int main(void) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW klass = {0};
    klass.lpfnWndProc = test_window_proc;
    klass.hInstance = instance;
    klass.lpszClassName = L"GoldensResourceWatcherTest";
    if (!RegisterClassW(&klass)) return 1;
    HWND window = CreateWindowW(klass.lpszClassName, L"watcher", WS_OVERLAPPED,
        0, 0, 100, 100, NULL, NULL, instance, NULL);
    if (!window) return 1;

    wchar_t temporary[MAX_PATH], seed[MAX_PATH], directory[MAX_PATH];
    if (!GetTempPathW(_countof(temporary), temporary) ||
        !GetTempFileNameW(temporary, L"glw", 0, seed)) return 1;
    DeleteFileW(seed);
    wcscpy(directory, seed);
    if (!CreateDirectoryW(directory, NULL)) return 1;

    GoldenResourceWatcher watcher = {0};
    int failed = golden_resource_watcher_start(NULL, directory, window,
                                                RESOURCE_CHANGED) ||
        golden_resource_watcher_start(&watcher, L"", window,
                                      RESOURCE_CHANGED) ||
        !golden_resource_watcher_start(&watcher, directory, window,
                                       RESOURCE_CHANGED) ||
        golden_resource_watcher_start(&watcher, directory, window,
                                      RESOURCE_CHANGED);

    wchar_t child[MAX_PATH], png[MAX_PATH], renamed[MAX_PATH];
    _snwprintf(child, _countof(child), L"%s\\nested", directory);
    _snwprintf(png, _countof(png), L"%s\\image.png", child);
    _snwprintf(renamed, _countof(renamed), L"%s\\renamed.png", child);
    if (!failed) failed = !CreateDirectoryW(child, NULL) || !wait_for_change(5000);
    LONG directory_generation = golden_resource_watcher_generation(&watcher);
    if (!failed) failed = directory_generation <= 0;
    HANDLE file = failed ? INVALID_HANDLE_VALUE : CreateFileW(
        png, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (!failed) failed = file == INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!failed) failed = !wait_for_new_generation(
        &watcher, directory_generation, 5000);
    if (!failed) failed = drain_changes() != 0;
    LONG file_generation = golden_resource_watcher_generation(&watcher);
    golden_resource_watcher_acknowledge(&watcher);
    if (!failed) failed = !MoveFileW(png, renamed) || !wait_for_change(5000);
    if (!failed) failed = golden_resource_watcher_generation(&watcher) <=
                          file_generation;
    golden_resource_watcher_acknowledge(&watcher);

    wchar_t burst_path[MAX_PATH];
    for (int i = 0; !failed && i < 2048; ++i) {
        _snwprintf(burst_path, _countof(burst_path),
                   L"%s\\burst-%04d.tmp", child, i);
        file = CreateFileW(burst_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE) failed = 1;
        else CloseHandle(file);
    }
    if (!failed) failed = !wait_for_change(5000);
    golden_resource_watcher_acknowledge(&watcher);
    _snwprintf(burst_path, _countof(burst_path),
               L"%s\\after-burst.tmp", child);
    file = failed ? INVALID_HANDLE_VALUE : CreateFileW(
        burst_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (!failed) failed = file == INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!failed) failed = !wait_for_change(5000);
    if (!golden_resource_watcher_stop(&watcher, 5000)) failed = 1;
    if (watcher.implementation) failed = 1;

    for (int i = 0; i < 2048; ++i) {
        _snwprintf(burst_path, _countof(burst_path),
                   L"%s\\burst-%04d.tmp", child, i);
        DeleteFileW(burst_path);
    }
    _snwprintf(burst_path, _countof(burst_path),
               L"%s\\after-burst.tmp", child);
    DeleteFileW(burst_path);
    DeleteFileW(png);
    DeleteFileW(renamed);
    RemoveDirectoryW(child);
    RemoveDirectoryW(directory);
    DestroyWindow(window);
    UnregisterClassW(klass.lpszClassName, instance);
    if (failed) return 1;
    puts("All Goldens resource watcher tests passed.");
    return 0;
}

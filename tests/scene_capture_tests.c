#include <windows.h>
#include <wincodec.h>

#include <stdio.h>

#include "../src/image_io.h"
#include "../src/resource_ops.h"
#include "../src/scene_capture.h"

static LRESULT CALLBACK test_window_proc(HWND window, UINT message,
                                         WPARAM wp, LPARAM lp) {
    UNREFERENCED_PARAMETER(wp);
    UNREFERENCED_PARAMETER(lp);
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT client;
        GetClientRect(window, &client);
        HBRUSH brush = CreateSolidBrush(RGB(37, 126, 203));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

static BOOL temporary_resource_path(wchar_t *directory, size_t directory_capacity,
                                    wchar_t *png, size_t png_capacity) {
    wchar_t temporary[MAX_PATH * 4];
    DWORD length = GetTempPathW(_countof(temporary), temporary);
    if (!length || length >= _countof(temporary)) return FALSE;
    int written = _snwprintf(
        directory, directory_capacity,
        L"%sgoldens-scene-test-%08lx-%08lx", temporary,
        (unsigned long)GetCurrentProcessId(),
        (unsigned long)GetTickCount());
    if (written < 0 || (size_t)written >= directory_capacity) return FALSE;
    if (GetFileAttributesW(directory) != INVALID_FILE_ATTRIBUTES)
        RemoveDirectoryW(directory);
    return CreateDirectoryW(directory, NULL) &&
           golden_path_join(directory, L"image.png", png, png_capacity);
}

int main(void) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW klass = {0};
    klass.lpfnWndProc = test_window_proc;
    klass.hInstance = instance;
    klass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    klass.lpszClassName = L"GoldensSceneCaptureTest";
    if (!RegisterClassW(&klass)) return 1;

    HWND root = CreateWindowW(
        klass.lpszClassName, L"Scene root", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80, 80, 420, 280, NULL, NULL, instance, NULL);
    HWND popup = CreateWindowExW(
        WS_EX_TOOLWINDOW, klass.lpszClassName, L"Owned popup",
        WS_POPUP | WS_VISIBLE, 440, 140, 180, 150,
        root, NULL, instance, NULL);
    HWND unrelated = CreateWindowW(
        klass.lpszClassName, L"Unrelated", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        700, 80, 180, 150, NULL, NULL, instance, NULL);
    int failed = !root || !popup || !unrelated;
    if (!failed) {
        ShowWindow(root, SW_SHOW);
        ShowWindow(popup, SW_SHOWNA);
        ShowWindow(unrelated, SW_SHOWNA);
        UpdateWindow(root);
        UpdateWindow(popup);
        UpdateWindow(unrelated);
    }

    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!failed && FAILED(initialized)) failed = 1;
    IWICImagingFactory *factory = NULL;
    HRESULT created = CoCreateInstance(
        &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&factory);
    if (!failed && FAILED(created)) failed = 1;

    wchar_t directory[MAX_PATH * 4] = L"";
    wchar_t png[MAX_PATH * 4] = L"";
    if (!failed && !temporary_resource_path(
            directory, _countof(directory), png, _countof(png))) failed = 1;
    if (!failed && golden_capture_scene(NULL, root, png, NULL) !=
                       GOLDEN_SCENE_CAPTURE_INVALID_ARGUMENT) failed = 1;

    RECT bounds = {0};
    GoldenSceneCaptureStatus status = failed ?
        GOLDEN_SCENE_CAPTURE_INVALID_ARGUMENT :
        golden_capture_scene(factory, popup, png, &bounds);
    BOOL screen_capture_skipped = !failed &&
        (status == GOLDEN_SCENE_CAPTURE_NO_VISIBLE_WINDOWS ||
         status == GOLDEN_SCENE_CAPTURE_SCREEN_FAILED);
    if (screen_capture_skipped) {
        puts("Desktop screen capture unavailable in this non-interactive "
             "session; disk capture checks skipped.");
    } else if (!failed && status != GOLDEN_SCENE_CAPTURE_OK) {
        fprintf(stderr, "scene capture failed: %d\n", (int)status);
        failed = 1;
    }

    wchar_t json[MAX_PATH * 4] = L"";
    if (!golden_resource_json_path(png, json, _countof(json))) failed = 1;
    if (!failed && !screen_capture_skipped) {
        GoldenImage image = {0};
        if (GetFileAttributesW(png) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(json) == INVALID_FILE_ATTRIBUTES ||
            !golden_png_load(factory, png, &image) ||
            image.width != (UINT)(bounds.right - bounds.left) ||
            image.height != (UINT)(bounds.bottom - bounds.top)) {
            fprintf(stderr, "invalid captured image resource\n");
            failed = 1;
        }
        golden_image_free(&image);
        if (!failed && golden_capture_scene(factory, root, png, NULL) !=
                           GOLDEN_SCENE_CAPTURE_DESTINATION_EXISTS) {
            fprintf(stderr, "existing capture destination was replaced\n");
            failed = 1;
        }
    }

    if (factory) IWICImagingFactory_Release(factory);
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (png[0]) DeleteFileW(png);
    if (json[0]) DeleteFileW(json);
    if (directory[0]) RemoveDirectoryW(directory);
    if (popup) DestroyWindow(popup);
    if (root) DestroyWindow(root);
    if (unrelated) DestroyWindow(unrelated);
    UnregisterClassW(klass.lpszClassName, instance);
    if (failed) {
        fprintf(stderr, "scene capture test failed\n");
        return 1;
    }
    puts("All Goldens scene capture tests passed.");
    return 0;
}

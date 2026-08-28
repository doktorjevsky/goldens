#include <windows.h>
#include <wincodec.h>

#include <stdio.h>
#include <wchar.h>

#include "../src/capture_bundle.h"
#include "../src/image_io.h"
#include "../src/resource_ops.h"

static LRESULT CALLBACK test_window_proc(HWND window, UINT message,
                                         WPARAM wp, LPARAM lp) {
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
    if (message == WM_PRINT || message == WM_PRINTCLIENT) {
        RECT bounds = {0};
        if (message == WM_PRINT) GetWindowRect(window, &bounds);
        else GetClientRect(window, &bounds);
        OffsetRect(&bounds, -bounds.left, -bounds.top);
        HBRUSH brush = CreateSolidBrush(RGB(37, 126, 203));
        FillRect((HDC)wp, &bounds, brush);
        DeleteObject(brush);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

static int scene_contains(const GoldenCaptureBundleScene *scene, HWND window) {
    for (size_t i = 0; i < scene->window_count; ++i)
        if (scene->windows[i].window == window) return 1;
    return 0;
}

static int path_exists(const wchar_t *directory, const wchar_t *leaf) {
    wchar_t path[MAX_PATH * 4];
    return golden_path_join(directory, leaf, path, _countof(path)) &&
           GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static int make_bundle_path(wchar_t *path, size_t capacity) {
    wchar_t temporary[MAX_PATH * 4];
    DWORD length = GetTempPathW(_countof(temporary), temporary);
    if (!length || length >= _countof(temporary)) return 0;
    int written = _snwprintf(path, capacity,
        L"%sgoldens-capture-bundle-test-%08lx", temporary,
        (unsigned long)GetCurrentProcessId());
    if (written < 0 || (size_t)written >= capacity) return 0;
    DWORD attributes = GetFileAttributesW(path);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !golden_delete_directory_tree(path)) return 0;
    }
    return 1;
}

int main(void) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW klass = {0};
    klass.lpfnWndProc = test_window_proc;
    klass.hInstance = instance;
    klass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    klass.lpszClassName = L"GoldensCaptureBundleTest";
    if (!RegisterClassW(&klass)) {
        fprintf(stderr, "could not register test window class: %lu\n",
                (unsigned long)GetLastError());
        return 1;
    }

    HWND root = CreateWindowW(klass.lpszClassName, L"Bundle root",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 420, 280,
        NULL, NULL, instance, NULL);
    HWND popup = CreateWindowExW(WS_EX_TOOLWINDOW, klass.lpszClassName,
        L"Owned popup", WS_POPUP | WS_VISIBLE, 440, 140, 180, 150,
        root, NULL, instance, NULL);
    HWND unrelated = CreateWindowW(klass.lpszClassName, L"Unrelated",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 650, 80, 180, 150,
        NULL, NULL, instance, NULL);
    if (!root || !popup || !unrelated) {
        fprintf(stderr, "could not create test windows: %lu\n",
                (unsigned long)GetLastError());
        return 2;
    }
    ShowWindow(root, SW_SHOW);
    ShowWindow(popup, SW_SHOWNA);
    ShowWindow(unrelated, SW_SHOWNA);
    SetWindowPos(root, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
    SetWindowPos(popup, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(unrelated, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowLongPtrW(root, GWL_STYLE,
                      GetWindowLongPtrW(root, GWL_STYLE) | WS_VISIBLE);
    SetWindowLongPtrW(popup, GWL_STYLE,
                      GetWindowLongPtrW(popup, GWL_STYLE) | WS_VISIBLE);
    SetWindowLongPtrW(unrelated, GWL_STYLE,
                      GetWindowLongPtrW(unrelated, GWL_STYLE) | WS_VISIBLE);
    UpdateWindow(root);
    UpdateWindow(popup);
    UpdateWindow(unrelated);

    GoldenCaptureBundleScene scene = {0};
    int failed = !golden_capture_bundle_collect_scene(popup, &scene);
    if (failed) {
        RECT root_debug = {0}, popup_debug = {0};
        GetWindowRect(root, &root_debug);
        GetWindowRect(popup, &popup_debug);
        fprintf(stderr,
            "could not collect visible window family: root=%p popup=%p "
            "owner=%p ancestor=%p visible=%d/%d iconic=%d/%d "
            "root_rect=%ld,%ld-%ld,%ld popup_rect=%ld,%ld-%ld,%ld "
            "desktop=%d,%d %dx%d\n",
            (void *)root, (void *)popup, (void *)GetWindow(popup, GW_OWNER),
            (void *)GetAncestor(popup, GA_ROOTOWNER),
            IsWindowVisible(root), IsWindowVisible(popup),
            IsIconic(root), IsIconic(popup),
            root_debug.left, root_debug.top, root_debug.right,
            root_debug.bottom, popup_debug.left, popup_debug.top,
            popup_debug.right, popup_debug.bottom,
            GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN));
    }
    if (!failed && (scene.root != root || scene.window_count != 2 ||
                    !scene_contains(&scene, root) ||
                    !scene_contains(&scene, popup) ||
                    scene_contains(&scene, unrelated))) {
        fprintf(stderr, "unexpected capture family\n");
        failed = 1;
    }
    for (size_t i = 0; !failed && i < scene.window_count; ++i)
        if (scene.scene_bounds.left > scene.windows[i].bounds.left ||
            scene.scene_bounds.top > scene.windows[i].bounds.top ||
            scene.scene_bounds.right < scene.windows[i].bounds.right ||
            scene.scene_bounds.bottom < scene.windows[i].bounds.bottom) {
            fprintf(stderr, "scene bounds do not contain the visible family\n");
            failed = 1;
        }
    golden_capture_bundle_release_scene(&scene);
    if (scene.windows || scene.window_count || scene.root) {
        fprintf(stderr, "scene release did not reset its result\n");
        failed = 1;
    }

    ShowWindow(popup, SW_HIDE);
    if (!failed && !golden_capture_bundle_collect_scene(root, &scene)) {
        fprintf(stderr, "could not collect root-only scene\n");
        failed = 1;
    }
    if (!failed && (scene.window_count != 1 || scene_contains(&scene, popup))) {
        fprintf(stderr, "hidden owned popup was captured\n");
        failed = 1;
    }
    golden_capture_bundle_release_scene(&scene);
    ShowWindow(popup, SW_SHOWNA);
    UpdateWindow(popup);

    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!failed && FAILED(initialized)) {
        fprintf(stderr, "could not initialize COM: 0x%08lx\n",
                (unsigned long)initialized);
        failed = 1;
    }
    IWICImagingFactory *factory = NULL;
    HRESULT created = CoCreateInstance(
        &CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void **)&factory);
    wchar_t bundle[MAX_PATH * 4];
    if (!failed && FAILED(created)) {
        fprintf(stderr, "could not create WIC factory: 0x%08lx\n",
                (unsigned long)created);
        failed = 1;
    }
    if (!failed && !make_bundle_path(bundle, _countof(bundle))) {
        fprintf(stderr, "could not prepare temporary bundle path\n");
        failed = 1;
    }
    SetForegroundWindow(root);
    Sleep(100);
    GoldenCaptureBundleResult result = {0};
    GoldenCaptureBundleStatus status = failed ?
        GOLDEN_CAPTURE_BUNDLE_INVALID_ARGUMENT :
        golden_capture_bundle_create(factory, popup, bundle, &result);
    int screen_capture_skipped =
        !failed && status == GOLDEN_CAPTURE_BUNDLE_SCREEN_CAPTURE_FAILED &&
        !IsWindowVisible(root);
    if (screen_capture_skipped)
        puts("Desktop screen capture unavailable in this non-interactive "
             "session; disk capture checks skipped.");
    else if (!failed && status != GOLDEN_CAPTURE_BUNDLE_OK) {
        fprintf(stderr, "bundle capture failed: %d\n", (int)status);
        failed = 1;
    }
    const wchar_t *expected[] = {
        L".goldens", L"scene.png", L"scene.json", L"root.png",
        L"root.json", L"window-01.png", L"window-01.json"
    };
    for (size_t i = 0;
         !failed && !screen_capture_skipped && i < _countof(expected); ++i) {
        if (!path_exists(bundle, expected[i])) {
            fwprintf(stderr, L"missing bundle file: %ls\n", expected[i]);
            failed = 1;
        }
    }
    GoldenImage image = {0};
    if (!failed && !screen_capture_skipped &&
        (!golden_png_load(factory, result.scene_path, &image) ||
         !image.width || !image.height || result.isolated_window_count != 2)) {
        fprintf(stderr, "invalid bundle result\n");
        failed = 1;
    }
    golden_image_free(&image);
    if (!failed && screen_capture_skipped && !CreateDirectoryW(bundle, NULL)) {
        fprintf(stderr, "could not create destination-exists fixture: %lu\n",
                (unsigned long)GetLastError());
        failed = 1;
    }
    if (!failed && golden_capture_bundle_create(
            factory, root, bundle, &result) !=
            GOLDEN_CAPTURE_BUNDLE_DESTINATION_EXISTS) {
        fprintf(stderr, "existing bundle destination was accepted\n");
        failed = 1;
    }

    if (GetFileAttributesW(bundle) != INVALID_FILE_ATTRIBUTES &&
        !golden_delete_directory_tree(bundle)) {
        fprintf(stderr, "could not clean captured bundle: %lu\n",
                (unsigned long)GetLastError());
        failed = 1;
    }
    if (factory) IWICImagingFactory_Release(factory);
    if (SUCCEEDED(initialized)) CoUninitialize();
    DestroyWindow(popup);
    DestroyWindow(root);
    DestroyWindow(unrelated);
    UnregisterClassW(klass.lpszClassName, instance);
    if (failed) {
        fprintf(stderr, "capture bundle test failed\n");
        return 1;
    }
    puts("All Goldens capture bundle tests passed.");
    return 0;
}

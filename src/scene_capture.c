#include "scene_capture.h"

#include "atomic_file.h"
#include "image_io.h"
#include "resource_ops.h"

#include <dwmapi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    HWND root;
    RECT bounds;
} SceneCollector;

static BOOL window_bounds(HWND window, RECT *bounds) {
    if (!window || !bounds || !IsWindow(window)) return FALSE;
    RECT dwm = {0};
    if (SUCCEEDED(DwmGetWindowAttribute(
            window, DWMWA_EXTENDED_FRAME_BOUNDS, &dwm, sizeof(dwm))) &&
        dwm.right > dwm.left && dwm.bottom > dwm.top) {
        *bounds = dwm;
        return TRUE;
    }
    return GetWindowRect(window, bounds) &&
           bounds->right > bounds->left && bounds->bottom > bounds->top;
}

static BOOL window_is_cloaked(HWND window) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(
        window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

static BOOL window_is_visible(HWND window) {
    return window && IsWindow(window) && !IsIconic(window) &&
           (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) &&
           !window_is_cloaked(window);
}

static BOOL window_is_owned_by(HWND window, HWND root) {
    HWND current = window;
    for (int depth = 0; depth < 128; ++depth) {
        current = GetWindow(current, GW_OWNER);
        if (!current) return FALSE;
        if (current == root) return TRUE;
        if (current == window) return FALSE;
    }
    return FALSE;
}

static void union_bounds(RECT *bounds, const RECT *addition) {
    bounds->left = min(bounds->left, addition->left);
    bounds->top = min(bounds->top, addition->top);
    bounds->right = max(bounds->right, addition->right);
    bounds->bottom = max(bounds->bottom, addition->bottom);
}

static BOOL CALLBACK collect_owned_window(HWND window, LPARAM parameter) {
    SceneCollector *collector = (SceneCollector *)parameter;
    if (window == collector->root || !window_is_visible(window) ||
        !window_is_owned_by(window, collector->root)) return TRUE;
    RECT bounds = {0};
    if (window_bounds(window, &bounds))
        union_bounds(&collector->bounds, &bounds);
    return TRUE;
}

static BOOL virtual_desktop_bounds(RECT *desktop) {
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (!desktop || width <= 0 || height <= 0) return FALSE;
    desktop->left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    desktop->top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    desktop->right = desktop->left + width;
    desktop->bottom = desktop->top + height;
    return TRUE;
}

static BOOL collect_scene_bounds(HWND foreground, RECT *bounds) {
    if (!foreground || !bounds || !window_is_visible(foreground)) return FALSE;
    HWND root = GetAncestor(foreground, GA_ROOTOWNER);
    if (!window_is_visible(root)) root = foreground;
    if (!window_bounds(root, bounds)) return FALSE;

    SceneCollector collector = {root, *bounds};
    SetLastError(ERROR_SUCCESS);
    if (!EnumWindows(collect_owned_window, (LPARAM)&collector) &&
        GetLastError() != ERROR_SUCCESS) return FALSE;
    RECT desktop = {0}, visible = collector.bounds;
    if (virtual_desktop_bounds(&desktop) &&
        !IntersectRect(&visible, &collector.bounds, &desktop)) return FALSE;
    LONGLONG width = (LONGLONG)visible.right - visible.left;
    LONGLONG height = (LONGLONG)visible.bottom - visible.top;
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 ||
        (uint64_t)width * (uint64_t)height > SIZE_MAX / 4u) return FALSE;
    *bounds = visible;
    return TRUE;
}

static BOOL capture_screen_rect(const RECT *bounds, GoldenImage *image) {
    int width = bounds->right - bounds->left;
    int height = bounds->bottom - bounds->top;
    HDC screen = GetDC(NULL);
    if (!screen) return FALSE;
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    BYTE *pixels = NULL;
    HBITMAP bitmap = CreateDIBSection(
        screen, &info, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    BOOL captured = FALSE;
    if (memory && bitmap && pixels) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        if (previous && previous != HGDI_ERROR) {
            captured = BitBlt(memory, 0, 0, width, height, screen,
                              bounds->left, bounds->top,
                              SRCCOPY | CAPTUREBLT);
            SelectObject(memory, previous);
        }
    }
    GoldenImage result = {0};
    if (captured) {
        size_t stride = (size_t)width * 4u;
        size_t bytes = stride * (size_t)height;
        result.pixels = (BYTE *)malloc(bytes);
        if (result.pixels) {
            memcpy(result.pixels, pixels, bytes);
            result.width = (UINT)width;
            result.height = (UINT)height;
            result.stride = (UINT)stride;
            golden_bgra_force_opaque(result.pixels, result.width,
                                     result.height, result.stride);
        } else captured = FALSE;
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(NULL, screen);
    if (!captured) {
        golden_image_free(&result);
        return FALSE;
    }
    *image = result;
    return TRUE;
}

GoldenSceneCaptureStatus golden_capture_scene(
    IWICImagingFactory *factory, HWND foreground, const wchar_t *png_path,
    RECT *captured_bounds) {
    if (!factory || !foreground || !png_path || !png_path[0])
        return GOLDEN_SCENE_CAPTURE_INVALID_ARGUMENT;
    wchar_t json_path[MAX_PATH * 4];
    if (!golden_resource_json_path(png_path, json_path,
                                   _countof(json_path)))
        return GOLDEN_SCENE_CAPTURE_INVALID_ARGUMENT;
    if (GetFileAttributesW(png_path) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(json_path) != INVALID_FILE_ATTRIBUTES)
        return GOLDEN_SCENE_CAPTURE_DESTINATION_EXISTS;

    GoldenImage image = {0};
    GoldenSceneCaptureStatus status = golden_capture_scene_image(
        foreground, &image, captured_bounds);
    if (status != GOLDEN_SCENE_CAPTURE_OK) return status;

    static const char empty_annotations[] =
        "{\n  \"annotations\": []\n}\n";
    BOOL png_saved = golden_png_save(
        factory, png_path, image.pixels, image.width, image.height,
        image.stride);
    BOOL json_saved = png_saved && golden_atomic_write_bytes(
        json_path, empty_annotations, sizeof(empty_annotations) - 1);
    golden_image_free(&image);
    if (!png_saved || !json_saved) {
        if (png_saved) DeleteFileW(png_path);
        if (json_saved) DeleteFileW(json_path);
        return GOLDEN_SCENE_CAPTURE_SAVE_FAILED;
    }
    return GOLDEN_SCENE_CAPTURE_OK;
}

GoldenSceneCaptureStatus golden_capture_scene_image(
    HWND foreground, GoldenImage *image, RECT *captured_bounds) {
    if (!foreground || !image) return GOLDEN_SCENE_CAPTURE_INVALID_ARGUMENT;
    RECT bounds = {0};
    if (!collect_scene_bounds(foreground, &bounds))
        return GOLDEN_SCENE_CAPTURE_NO_VISIBLE_WINDOWS;
    GoldenImage captured = {0};
    if (!capture_screen_rect(&bounds, &captured))
        return GOLDEN_SCENE_CAPTURE_SCREEN_FAILED;
    golden_image_free(image);
    *image = captured;
    if (captured_bounds) *captured_bounds = bounds;
    return GOLDEN_SCENE_CAPTURE_OK;
}

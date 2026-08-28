#include "capture_bundle.h"

#include "atomic_file.h"
#include "image_io.h"
#include "window_capture.h"
#include "resource_ops.h"

#include <dwmapi.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    HWND root;
    GoldenCaptureBundleWindow *windows;
    size_t count;
    size_t capacity;
    BOOL failed;
} SceneCollector;

static volatile LONG staging_sequence;

static BOOL capture_bounds(HWND window, RECT *bounds) {
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

static BOOL window_has_visible_style(HWND window) {
    return (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
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

static BOOL rect_intersects_desktop(const RECT *bounds) {
    RECT desktop;
    if (!virtual_desktop_bounds(&desktop)) return TRUE;
    RECT intersection;
    return IntersectRect(&intersection, bounds, &desktop);
}

static BOOL append_window(SceneCollector *collector,
                          const GoldenCaptureBundleWindow *window) {
    if (collector->count == collector->capacity) {
        size_t capacity = collector->capacity ? collector->capacity * 2 : 8;
        if (capacity < collector->capacity ||
            capacity > SIZE_MAX / sizeof(*collector->windows)) return FALSE;
        GoldenCaptureBundleWindow *windows =
            (GoldenCaptureBundleWindow *)realloc(
                collector->windows, capacity * sizeof(*windows));
        if (!windows) return FALSE;
        collector->windows = windows;
        collector->capacity = capacity;
    }
    collector->windows[collector->count++] = *window;
    return TRUE;
}

static BOOL CALLBACK collect_owned_window(HWND window, LPARAM parameter) {
    SceneCollector *collector = (SceneCollector *)parameter;
    if (window == collector->root || !window_has_visible_style(window) ||
        IsIconic(window) || window_is_cloaked(window) ||
        !window_is_owned_by(window, collector->root)) return TRUE;
    GoldenCaptureBundleWindow item = {0};
    item.window = window;
    if (!capture_bounds(window, &item.bounds) ||
        !rect_intersects_desktop(&item.bounds)) return TRUE;
    if (!append_window(collector, &item)) {
        collector->failed = TRUE;
        return FALSE;
    }
    return TRUE;
}

static void union_bounds(RECT *bounds, const RECT *addition) {
    bounds->left = min(bounds->left, addition->left);
    bounds->top = min(bounds->top, addition->top);
    bounds->right = max(bounds->right, addition->right);
    bounds->bottom = max(bounds->bottom, addition->bottom);
}

BOOL golden_capture_bundle_collect_scene(HWND foreground,
                                         GoldenCaptureBundleScene *scene) {
    if (!scene || !foreground || !IsWindow(foreground) ||
        !window_has_visible_style(foreground) || IsIconic(foreground)) return FALSE;
    *scene = (GoldenCaptureBundleScene){0};
    HWND root = GetAncestor(foreground, GA_ROOTOWNER);
    if (!root || !window_has_visible_style(root) || IsIconic(root)) root = foreground;

    GoldenCaptureBundleWindow root_window = {0};
    root_window.window = root;
    if (!capture_bounds(root, &root_window.bounds) ||
        !rect_intersects_desktop(&root_window.bounds)) return FALSE;

    SceneCollector collector = {0};
    collector.root = root;
    if (!append_window(&collector, &root_window)) return FALSE;
    EnumWindows(collect_owned_window, (LPARAM)&collector);
    if (collector.failed) {
        free(collector.windows);
        return FALSE;
    }

    RECT union_rect = collector.windows[0].bounds;
    for (size_t i = 1; i < collector.count; ++i)
        union_bounds(&union_rect, &collector.windows[i].bounds);
    RECT desktop;
    RECT visible_union = union_rect;
    if (virtual_desktop_bounds(&desktop) &&
        !IntersectRect(&visible_union, &union_rect, &desktop)) {
            free(collector.windows);
            return FALSE;
        }
    LONGLONG width = (LONGLONG)visible_union.right - visible_union.left;
    LONGLONG height = (LONGLONG)visible_union.bottom - visible_union.top;
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 ||
        (uint64_t)width * (uint64_t)height > SIZE_MAX / 4u) {
        free(collector.windows);
        return FALSE;
    }

    scene->root = root;
    scene->windows = collector.windows;
    scene->window_count = collector.count;
    scene->scene_bounds = visible_union;
    return TRUE;
}

void golden_capture_bundle_release_scene(GoldenCaptureBundleScene *scene) {
    if (!scene) return;
    free(scene->windows);
    *scene = (GoldenCaptureBundleScene){0};
}

static BOOL capture_screen_rect(const RECT *bounds, GoldenImage *image) {
    if (!bounds || !image) return FALSE;
    int width = bounds->right - bounds->left;
    int height = bounds->bottom - bounds->top;
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768 ||
        (size_t)width > SIZE_MAX / 4u ||
        (size_t)width * 4u > SIZE_MAX / (size_t)height) return FALSE;

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
        HGDIOBJ old = SelectObject(memory, bitmap);
        if (old && old != HGDI_ERROR)
            captured = BitBlt(memory, 0, 0, width, height, screen,
                              bounds->left, bounds->top,
                              SRCCOPY | CAPTUREBLT);
        if (old && old != HGDI_ERROR) SelectObject(memory, old);
    }
    if (captured) {
        size_t stride = (size_t)width * 4u;
        size_t bytes = stride * (size_t)height;
        BYTE *copy = (BYTE *)malloc(bytes);
        if (copy) {
            memcpy(copy, pixels, bytes);
            golden_bgra_force_opaque(copy, (UINT)width, (UINT)height,
                                     (UINT)stride);
            *image = (GoldenImage){copy, (UINT)width, (UINT)height,
                                  (UINT)stride};
        } else captured = FALSE;
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(NULL, screen);
    return captured;
}

static BOOL copy_scene_crop(const GoldenImage *scene, const RECT *scene_bounds,
                            const RECT *window_bounds, GoldenImage *crop) {
    RECT visible;
    if (!IntersectRect(&visible, scene_bounds, window_bounds)) return FALSE;
    UINT width = (UINT)(visible.right - visible.left);
    UINT height = (UINT)(visible.bottom - visible.top);
    if (!width || !height || width > UINT_MAX / 4u ||
        (size_t)width * 4u > SIZE_MAX / height) return FALSE;
    UINT stride = width * 4u;
    BYTE *pixels = (BYTE *)malloc((size_t)stride * height);
    if (!pixels) return FALSE;
    size_t source_x = (size_t)(visible.left - scene_bounds->left) * 4u;
    size_t source_y = (size_t)(visible.top - scene_bounds->top);
    for (UINT y = 0; y < height; ++y)
        memcpy(pixels + (size_t)y * stride,
               scene->pixels + (source_y + y) * scene->stride + source_x,
               stride);
    *crop = (GoldenImage){pixels, width, height, stride};
    return TRUE;
}

static BOOL make_staging_path(const wchar_t *destination,
                              wchar_t *staging, size_t capacity) {
    wchar_t parent[GOLDEN_CAPTURE_BUNDLE_PATH_CAPACITY];
    if (!golden_path_copy(destination, parent, _countof(parent))) return FALSE;
    wchar_t *slash = wcsrchr(parent, L'\\');
    wchar_t *forward = wcsrchr(parent, L'/');
    if (!slash || (forward && forward > slash)) slash = forward;
    if (!slash) return FALSE;
    *slash = 0;
    for (int attempt = 0; attempt < 128; ++attempt) {
        LONG sequence = InterlockedIncrement(&staging_sequence);
        wchar_t leaf[96];
        int length = _snwprintf(
            leaf, _countof(leaf), L".goldens-capture-%08lx-%08lx",
            (unsigned long)GetCurrentProcessId(), (unsigned long)sequence);
        if (length < 0 || (size_t)length >= _countof(leaf) ||
            !golden_path_join(parent, leaf, staging, capacity)) return FALSE;
        if (CreateDirectoryW(staging, NULL)) {
            SetFileAttributesW(staging, FILE_ATTRIBUTE_HIDDEN);
            return TRUE;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    }
    return FALSE;
}

static BOOL save_empty_annotations(const wchar_t *directory,
                                   const wchar_t *stem) {
    wchar_t path[GOLDEN_CAPTURE_BUNDLE_PATH_CAPACITY];
    static const char empty[] = "{\n  \"annotations\": []\n}\n";
    return golden_path_join_extension(directory, stem, L".json", path,
                                      _countof(path)) &&
           golden_atomic_write_bytes(path, empty, sizeof(empty) - 1);
}

static BOOL save_resource(IWICImagingFactory *factory,
                          const wchar_t *directory, const wchar_t *stem,
                          const GoldenImage *image) {
    wchar_t path[GOLDEN_CAPTURE_BUNDLE_PATH_CAPACITY];
    return golden_path_join_extension(directory, stem, L".png", path,
                                      _countof(path)) &&
           golden_png_save(factory, path, image->pixels, image->width,
                           image->height, image->stride) &&
           save_empty_annotations(directory, stem);
}

static BOOL write_manifest(const wchar_t *directory, size_t window_count) {
    if (window_count > (SIZE_MAX - 512u) / 96u) return FALSE;
    size_t capacity = 512u + window_count * 96u;
    char *json = (char *)malloc(capacity);
    if (!json) return FALSE;
    size_t used = 0;
    int written = snprintf(json + used, capacity - used,
        "{\n  \"version\": 1,\n  \"kind\": \"capture-bundle\",\n"
        "  \"composite\": \"scene.png\",\n  \"assets\": [\n"
        "    {\"path\": \"scene.png\", \"kind\": \"composite\"}");
    if (written < 0 || (size_t)written >= capacity - used) {
        free(json);
        return FALSE;
    }
    used += (size_t)written;
    for (size_t i = 0; i < window_count; ++i) {
        char path[48];
        if (!i) strcpy(path, "root.png");
        else snprintf(path, sizeof(path), "window-%02lu.png",
                      (unsigned long)i);
        written = snprintf(json + used, capacity - used,
            ",\n    {\"path\": \"%s\", \"kind\": \"%s\"}", path,
            !i ? "root-window" : "owned-window");
        if (written < 0 || (size_t)written >= capacity - used) {
            free(json);
            return FALSE;
        }
        used += (size_t)written;
    }
    written = snprintf(json + used, capacity - used, "\n  ]\n}\n");
    if (written < 0 || (size_t)written >= capacity - used) {
        free(json);
        return FALSE;
    }
    used += (size_t)written;
    wchar_t path[GOLDEN_CAPTURE_BUNDLE_PATH_CAPACITY];
    BOOL saved = golden_path_join(directory, L".goldens", path,
                                  _countof(path)) &&
                 golden_atomic_write_bytes(path, json, used);
    free(json);
    return saved;
}

GoldenCaptureBundleStatus golden_capture_bundle_create(
    IWICImagingFactory *factory, HWND foreground,
    const wchar_t *destination, GoldenCaptureBundleResult *result) {
    if (result) *result = (GoldenCaptureBundleResult){0};
    if (!factory || !foreground || !destination || !destination[0] ||
        !result) return GOLDEN_CAPTURE_BUNDLE_INVALID_ARGUMENT;
    wchar_t final_scene_path[GOLDEN_CAPTURE_BUNDLE_PATH_CAPACITY];
    if (!golden_path_join_extension(destination, L"scene", L".png",
                                    final_scene_path,
                                    _countof(final_scene_path)))
        return GOLDEN_CAPTURE_BUNDLE_INVALID_ARGUMENT;
    if (GetFileAttributesW(destination) != INVALID_FILE_ATTRIBUTES)
        return GOLDEN_CAPTURE_BUNDLE_DESTINATION_EXISTS;

    GoldenCaptureBundleScene scene = {0};
    if (!golden_capture_bundle_collect_scene(foreground, &scene))
        return GOLDEN_CAPTURE_BUNDLE_NO_VISIBLE_WINDOWS;

    wchar_t staging[GOLDEN_CAPTURE_BUNDLE_PATH_CAPACITY];
    if (!make_staging_path(destination, staging, _countof(staging))) {
        golden_capture_bundle_release_scene(&scene);
        return GOLDEN_CAPTURE_BUNDLE_CREATE_FAILED;
    }

    GoldenImage composite = {0};
    GoldenCaptureBundleStatus status = GOLDEN_CAPTURE_BUNDLE_OK;
    if (!capture_screen_rect(&scene.scene_bounds, &composite))
        status = GOLDEN_CAPTURE_BUNDLE_SCREEN_CAPTURE_FAILED;
    if (status == GOLDEN_CAPTURE_BUNDLE_OK &&
        !save_resource(factory, staging, L"scene", &composite))
        status = GOLDEN_CAPTURE_BUNDLE_SAVE_FAILED;

    for (size_t i = 0;
         status == GOLDEN_CAPTURE_BUNDLE_OK && i < scene.window_count; ++i) {
        GoldenImage isolated = {0};
        /* PrintWindow frequently renders DWM-owned shadows for popup windows
           as solid black. Owned windows are already visible in the desktop
           composite, so crop them from that source exactly as the user sees
           them. Keep PrintWindow first for the root so overlays do not become
           part of the otherwise isolated root asset. */
        BOOL captured = i == 0 ?
            (golden_capture_window(scene.windows[i].window, &isolated) ||
             copy_scene_crop(&composite, &scene.scene_bounds,
                             &scene.windows[i].bounds, &isolated)) :
            (copy_scene_crop(&composite, &scene.scene_bounds,
                             &scene.windows[i].bounds, &isolated) ||
             golden_capture_window(scene.windows[i].window, &isolated));
        if (!captured) {
            status = GOLDEN_CAPTURE_BUNDLE_SCREEN_CAPTURE_FAILED;
            break;
        }
        wchar_t stem[48];
        if (!i) wcscpy(stem, L"root");
        else _snwprintf(stem, _countof(stem), L"window-%02lu",
                        (unsigned long)i);
        if (!save_resource(factory, staging, stem, &isolated))
            status = GOLDEN_CAPTURE_BUNDLE_SAVE_FAILED;
        golden_image_free(&isolated);
    }
    if (status == GOLDEN_CAPTURE_BUNDLE_OK &&
        !write_manifest(staging, scene.window_count))
        status = GOLDEN_CAPTURE_BUNDLE_MANIFEST_FAILED;

    if (status == GOLDEN_CAPTURE_BUNDLE_OK) {
        SetFileAttributesW(staging, FILE_ATTRIBUTE_NORMAL);
        if (!MoveFileExW(staging, destination, MOVEFILE_WRITE_THROUGH))
            status = GOLDEN_CAPTURE_BUNDLE_FINALIZE_FAILED;
    }
    if (status != GOLDEN_CAPTURE_BUNDLE_OK)
        golden_delete_directory_tree(staging);

    if (status == GOLDEN_CAPTURE_BUNDLE_OK) {
        golden_path_copy(final_scene_path, result->scene_path,
                         _countof(result->scene_path));
        result->isolated_window_count = scene.window_count > UINT_MAX ?
            UINT_MAX : (UINT)scene.window_count;
        result->scene_bounds = scene.scene_bounds;
    }
    golden_image_free(&composite);
    golden_capture_bundle_release_scene(&scene);
    return status;
}

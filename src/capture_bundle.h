#ifndef GOLDENS_CAPTURE_BUNDLE_H
#define GOLDENS_CAPTURE_BUNDLE_H

#include <windows.h>
#include <wincodec.h>

#include <stddef.h>

#define GOLDEN_CAPTURE_BUNDLE_PATH_CAPACITY (MAX_PATH * 4)

typedef struct {
    HWND window;
    RECT bounds;
} GoldenCaptureBundleWindow;

typedef struct {
    HWND root;
    GoldenCaptureBundleWindow *windows;
    size_t window_count;
    RECT scene_bounds;
} GoldenCaptureBundleScene;

typedef enum {
    GOLDEN_CAPTURE_BUNDLE_OK,
    GOLDEN_CAPTURE_BUNDLE_INVALID_ARGUMENT,
    GOLDEN_CAPTURE_BUNDLE_DESTINATION_EXISTS,
    GOLDEN_CAPTURE_BUNDLE_CREATE_FAILED,
    GOLDEN_CAPTURE_BUNDLE_NO_VISIBLE_WINDOWS,
    GOLDEN_CAPTURE_BUNDLE_SCREEN_CAPTURE_FAILED,
    GOLDEN_CAPTURE_BUNDLE_SAVE_FAILED,
    GOLDEN_CAPTURE_BUNDLE_MANIFEST_FAILED,
    GOLDEN_CAPTURE_BUNDLE_FINALIZE_FAILED
} GoldenCaptureBundleStatus;

typedef struct {
    wchar_t scene_path[GOLDEN_CAPTURE_BUNDLE_PATH_CAPACITY];
    UINT isolated_window_count;
    RECT scene_bounds;
} GoldenCaptureBundleResult;

BOOL golden_capture_bundle_collect_scene(HWND foreground,
                                         GoldenCaptureBundleScene *scene);
void golden_capture_bundle_release_scene(GoldenCaptureBundleScene *scene);

GoldenCaptureBundleStatus golden_capture_bundle_create(
    IWICImagingFactory *factory, HWND foreground,
    const wchar_t *destination, GoldenCaptureBundleResult *result);

#endif

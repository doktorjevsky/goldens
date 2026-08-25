#include <windows.h>
#include <dwmapi.h>

#include <stdio.h>

#include "../src/preview_capture.h"

static LRESULT CALLBACK test_window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT client;
        GetClientRect(window, &client);
        HBRUSH brush = CreateSolidBrush(RGB(31, 119, 203));
        FillRect(dc, &client, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_PRINT || message == WM_PRINTCLIENT) {
        RECT client = {0};
        if (message == WM_PRINT) GetWindowRect(window, &client);
        else GetClientRect(window, &client);
        OffsetRect(&client, -client.left, -client.top);
        HBRUSH brush = CreateSolidBrush(RGB(31, 119, 203));
        FillRect((HDC)wp, &client, brush);
        DeleteObject(brush);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

static BOOL render_test_preview(HWND window, HDC destination, const RECT *bounds,
                                BYTE *pixels, int width, int height, void *context) {
    UNREFERENCED_PARAMETER(bounds);
    UNREFERENCED_PARAMETER(pixels);
    UNREFERENCED_PARAMETER(context);
    PatBlt(destination, 0, 0, width, height, BLACKNESS);
    RECT window_bounds = {0}, visible_bounds = {0}, source = {0};
    if (!GetWindowRect(window, &window_bounds)) return FALSE;
    visible_bounds = window_bounds;
    RECT dwm_bounds = {0};
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &dwm_bounds, sizeof(dwm_bounds))) &&
        golden_preview_source_rect(&window_bounds, &dwm_bounds, &source))
        visible_bounds = dwm_bounds;
    if (!golden_preview_source_rect(&window_bounds, &visible_bounds, &source))
        return FALSE;
    HBRUSH brush = CreateSolidBrush(RGB(31, 119, 203));
    FillRect(destination, &source, brush);
    DeleteObject(brush);
    return TRUE;
}

static BOOL fail_preview(HWND window, HDC destination, const RECT *bounds,
                         BYTE *pixels, int width, int height, void *context) {
    UNREFERENCED_PARAMETER(window);
    UNREFERENCED_PARAMETER(destination);
    UNREFERENCED_PARAMETER(bounds);
    UNREFERENCED_PARAMETER(pixels);
    UNREFERENCED_PARAMETER(width);
    UNREFERENCED_PARAMETER(height);
    UNREFERENCED_PARAMETER(context);
    return FALSE;
}

static int pixel_is_test_blue(const GoldenImage *image, UINT x, UINT y) {
    const BYTE *pixel = image->pixels + (size_t)y * image->stride + (size_t)x * 4;
    return pixel[0] >= 185 && pixel[0] <= 220 &&
           pixel[1] >= 105 && pixel[1] <= 135 &&
           pixel[2] >= 20 && pixel[2] <= 45;
}

static int image_edges_are_test_blue(const GoldenImage *image) {
    UINT points[][2] = {
        {0, 0}, {image->width - 1, 0},
        {0, image->height - 1}, {image->width - 1, image->height - 1},
        {image->width / 2, image->height / 2}
    };
    for (size_t i = 0; i < _countof(points); ++i)
        if (!pixel_is_test_blue(image, points[i][0], points[i][1])) return 0;
    return 1;
}

int main(void) {
    RECT window_bounds = {100, 200, 436, 748};
    RECT visible_bounds = {107, 200, 429, 734};
    RECT source = {0};
    int failed = !golden_preview_source_rect(&window_bounds, &visible_bounds, &source);
    if (!failed && (source.left != 7 || source.top != 0 ||
                    source.right != 329 || source.bottom != 534)) {
        fprintf(stderr, "unexpected visible-frame crop: %ld,%ld - %ld,%ld\n",
                source.left, source.top, source.right, source.bottom);
        failed = 1;
    }
    RECT outside_bounds = {99, 200, 429, 734};
    if (!failed && golden_preview_source_rect(&window_bounds, &outside_bounds, &source)) {
        fprintf(stderr, "accepted visible bounds outside the printed window\n");
        failed = 1;
    }

    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW klass = {0};
    klass.lpfnWndProc = test_window_proc;
    klass.hInstance = instance;
    klass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    klass.lpszClassName = L"GoldensPreviewCaptureTest";
    if (!RegisterClassW(&klass)) return 1;
    HWND window = CreateWindowW(klass.lpszClassName, L"Preview capture test",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 360, 240,
        NULL, NULL, instance, NULL);
    if (!window) return 1;
    UpdateWindow(window);

    GoldenImage image = {0};
    if (!failed) failed = !golden_capture_window_preview_with_renderer(
        window, &image, render_test_preview, NULL);
    if (failed) fprintf(stderr, "window capture returned false\n");
    if (!failed) {
        failed = image.width < 300 || image.height < 180 ||
                 image.stride != image.width * 4;
        if (failed) fprintf(stderr, "unexpected preview geometry: %u x %u, stride %u\n",
                            image.width, image.height, image.stride);
        if (!failed && !image_edges_are_test_blue(&image)) {
            fprintf(stderr, "invisible frame pixels survived compatibility capture\n");
            failed = 1;
        }
    }
    golden_image_free(&image);

    GoldenPreviewSurface surface = {0};
    if (!failed && !golden_preview_surface_capture_with_renderer(
            &surface, window, render_test_preview, NULL)) failed = 1;
    HBITMAP first_bitmap = surface.bitmap;
    BYTE *first_pixels = surface.pixels;
    UINT first_width = surface.width, first_height = surface.height;
    UINT first_bitmap_width = surface.bitmap_width;
    UINT first_bitmap_height = surface.bitmap_height;
    GoldenImage view = golden_preview_surface_image(&surface);
    if (!failed && !image_edges_are_test_blue(&view)) failed = 1;
    if (!failed && !golden_preview_surface_capture_with_renderer(
            &surface, window, render_test_preview, NULL)) failed = 1;
    if (!failed && (surface.bitmap != first_bitmap ||
                    surface.pixels != first_pixels ||
                    surface.width != first_width ||
                    surface.height != first_height ||
                    surface.bitmap_width != first_bitmap_width ||
                    surface.bitmap_height != first_bitmap_height)) failed = 1;
    view = golden_preview_surface_image(&surface);
    if (!failed && (view.pixels != surface.pixels || view.width != surface.width ||
                    view.height != surface.height || view.stride != surface.stride ||
                    !image_edges_are_test_blue(&view))) failed = 1;
    if (!failed && golden_preview_surface_capture_with_renderer(
            &surface, window, fail_preview, NULL)) failed = 1;
    if (!failed && (surface.bitmap != first_bitmap ||
                    surface.pixels != first_pixels)) failed = 1;
    SetWindowPos(window, NULL, 0, 0, 480, 320,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!failed && !golden_preview_surface_capture_with_renderer(
            &surface, window, render_test_preview, NULL)) failed = 1;
    if (!failed && (surface.width == first_width || surface.height == first_height ||
                    surface.bitmap_width == first_bitmap_width ||
                    surface.bitmap_height == first_bitmap_height)) failed = 1;
    golden_preview_surface_release(&surface);
    if (surface.memory || surface.bitmap || surface.pixels ||
        surface.width || surface.height || surface.stride ||
        surface.bitmap_width || surface.bitmap_height) failed = 1;

    DestroyWindow(window);
    UnregisterClassW(klass.lpszClassName, instance);
    if (failed) return 1;
    puts("All Goldens window preview tests passed.");
    return 0;
}

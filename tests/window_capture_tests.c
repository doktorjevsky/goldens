#include <windows.h>
#include <dwmapi.h>

#include <stdio.h>

#include "../src/window_capture.h"

static LRESULT CALLBACK test_window_proc(HWND window, UINT message,
                                         WPARAM wp, LPARAM lp) {
    UNREFERENCED_PARAMETER(lp);
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
    return DefWindowProcW(window, message, wp, lp);
}

static BOOL render_test_window(HWND window, HDC destination,
                               const RECT *bounds, BYTE *pixels,
                               int width, int height, void *context) {
    UNREFERENCED_PARAMETER(pixels);
    UNREFERENCED_PARAMETER(context);
    PatBlt(destination, 0, 0, width, height, BLACKNESS);
    RECT visible = *bounds, source = {0}, dwm = {0};
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &dwm, sizeof(dwm))) &&
        golden_window_capture_source_rect(bounds, &dwm, &source))
        visible = dwm;
    if (!golden_window_capture_source_rect(bounds, &visible, &source))
        return FALSE;
    HBRUSH brush = CreateSolidBrush(RGB(31, 119, 203));
    FillRect(destination, &source, brush);
    DeleteObject(brush);
    return TRUE;
}

static BOOL fail_capture(HWND window, HDC destination, const RECT *bounds,
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
    const BYTE *pixel = image->pixels + (size_t)y * image->stride +
                        (size_t)x * 4u;
    return pixel[0] >= 185 && pixel[0] <= 220 &&
           pixel[1] >= 105 && pixel[1] <= 135 &&
           pixel[2] >= 20 && pixel[2] <= 45 && pixel[3] == 255;
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
    int failed = !golden_window_capture_source_rect(
        &window_bounds, &visible_bounds, &source);
    if (!failed && (source.left != 7 || source.top != 0 ||
                    source.right != 329 || source.bottom != 534)) {
        fprintf(stderr, "unexpected visible-frame crop\n");
        failed = 1;
    }
    RECT outside_bounds = {99, 200, 429, 734};
    if (!failed && golden_window_capture_source_rect(
            &window_bounds, &outside_bounds, &source)) {
        fprintf(stderr, "accepted visible bounds outside the window\n");
        failed = 1;
    }

    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW klass = {0};
    klass.lpfnWndProc = test_window_proc;
    klass.hInstance = instance;
    klass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    klass.lpszClassName = L"GoldensWindowCaptureTest";
    if (!RegisterClassW(&klass)) return 1;
    HWND window = CreateWindowW(
        klass.lpszClassName, L"Window capture test",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 360, 240,
        NULL, NULL, instance, NULL);
    if (!window) return 1;
    UpdateWindow(window);

    GoldenImage image = {0};
    if (!failed) failed = !golden_capture_window_with_renderer(
        window, &image, render_test_window, NULL);
    if (!failed && (image.width < 300 || image.height < 180 ||
                    image.stride != image.width * 4u ||
                    !image_edges_are_test_blue(&image))) {
        fprintf(stderr, "invalid captured window image\n");
        failed = 1;
    }
    golden_image_free(&image);
    if (!failed && golden_capture_window_with_renderer(
            window, &image, fail_capture, NULL)) {
        fprintf(stderr, "failed renderer was accepted\n");
        failed = 1;
    }

    DestroyWindow(window);
    UnregisterClassW(klass.lpszClassName, instance);
    if (failed) return 1;
    puts("All Goldens window capture tests passed.");
    return 0;
}

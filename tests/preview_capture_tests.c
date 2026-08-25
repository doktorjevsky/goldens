#include <windows.h>

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
    UNREFERENCED_PARAMETER(window);
    UNREFERENCED_PARAMETER(bounds);
    UNREFERENCED_PARAMETER(pixels);
    UNREFERENCED_PARAMETER(context);
    HBRUSH brush = CreateSolidBrush(RGB(31, 119, 203));
    RECT target = {0, 0, width, height};
    FillRect(destination, &target, brush);
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

int main(void) {
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
    int failed = !golden_capture_window_preview_with_renderer(
        window, &image, render_test_preview, NULL);
    if (failed) fprintf(stderr, "window capture returned false\n");
    if (!failed) {
        failed = image.width < 300 || image.height < 180 || image.stride != image.width * 4;
        if (failed) fprintf(stderr, "unexpected preview geometry: %u x %u, stride %u\n",
                            image.width, image.height, image.stride);
        size_t center = ((size_t)image.height / 2 * image.stride) + (image.width / 2 * 4);
        if (!failed) {
            BYTE blue = image.pixels[center + 0];
            BYTE green = image.pixels[center + 1];
            BYTE red = image.pixels[center + 2];
            failed = red < 20 || red > 45 || green < 105 || green > 135 || blue < 185 || blue > 220;
            if (failed) fprintf(stderr, "unexpected center BGRA: %u %u %u %u\n",
                                blue, green, red, image.pixels[center + 3]);
        }
    }
    golden_image_free(&image);

    GoldenPreviewSurface surface = {0};
    if (!failed && !golden_preview_surface_capture_with_renderer(
            &surface, window, render_test_preview, NULL)) failed = 1;
    HBITMAP first_bitmap = surface.bitmap;
    BYTE *first_pixels = surface.pixels;
    UINT first_width = surface.width, first_height = surface.height;
    if (!failed && !golden_preview_surface_capture_with_renderer(
            &surface, window, render_test_preview, NULL)) failed = 1;
    if (!failed && (surface.bitmap != first_bitmap ||
                    surface.pixels != first_pixels ||
                    surface.width != first_width ||
                    surface.height != first_height)) failed = 1;
    GoldenImage view = golden_preview_surface_image(&surface);
    if (!failed && (view.pixels != surface.pixels || view.width != surface.width ||
                    view.height != surface.height || view.stride != surface.stride)) failed = 1;
    if (!failed && golden_preview_surface_capture_with_renderer(
            &surface, window, fail_preview, NULL)) failed = 1;
    if (!failed && (surface.bitmap != first_bitmap || surface.pixels != first_pixels)) failed = 1;
    SetWindowPos(window, NULL, 0, 0, 480, 320, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!failed && !golden_preview_surface_capture_with_renderer(
            &surface, window, render_test_preview, NULL)) failed = 1;
    if (!failed && (surface.width == first_width || surface.height == first_height)) failed = 1;
    golden_preview_surface_release(&surface);
    if (surface.memory || surface.bitmap || surface.pixels ||
        surface.width || surface.height || surface.stride) failed = 1;

    DestroyWindow(window);
    UnregisterClassW(klass.lpszClassName, instance);
    if (failed) return 1;
    puts("All Goldens window preview tests passed.");
    return 0;
}

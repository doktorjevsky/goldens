#include <windows.h>

#include <stdio.h>

#include "../src/preview_service.h"

#define PREVIEW_READY (WM_APP + 42)

typedef struct {
    HANDLE first_started;
    HANDLE release_first;
    LONG calls;
    DWORD thread_ids[8];
    HWND targets[8];
    HBITMAP bitmaps[8];
    BOOL fail_next;
} CaptureContext;

static BOOL fill_surface(HWND window, HDC destination, const RECT *bounds,
                         BYTE *pixels, int width, int height, void *context) {
    UNREFERENCED_PARAMETER(window);
    UNREFERENCED_PARAMETER(bounds);
    UNREFERENCED_PARAMETER(pixels);
    UNREFERENCED_PARAMETER(context);
    RECT area = {0, 0, width, height};
    FillRect(destination, &area, GetStockObject(WHITE_BRUSH));
    return TRUE;
}

static BOOL controlled_capture(HWND target, GoldenPreviewSurface *surface,
                               void *opaque) {
    CaptureContext *context = (CaptureContext *)opaque;
    LONG index = InterlockedIncrement(&context->calls) - 1;
    if (index < (LONG)_countof(context->thread_ids)) {
        context->thread_ids[index] = GetCurrentThreadId();
        context->targets[index] = target;
    }
    if (!index) {
        SetEvent(context->first_started);
        WaitForSingleObject(context->release_first, 5000);
    }
    if (context->fail_next) {
        context->fail_next = FALSE;
        return FALSE;
    }
    BOOL captured = golden_preview_surface_capture_with_renderer(
        surface, target, fill_surface, NULL);
    if (captured && index < (LONG)_countof(context->bitmaps))
        context->bitmaps[index] = surface->bitmap;
    return captured;
}

static LRESULT CALLBACK test_window_proc(HWND window, UINT message,
                                         WPARAM wp, LPARAM lp) {
    return DefWindowProcW(window, message, wp, lp);
}

static int wait_for_preview_message(DWORD timeout_ms) {
    DWORD start = GetTickCount();
    for (;;) {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == PREVIEW_READY) return 1;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeout_ms) return 0;
        MsgWaitForMultipleObjects(0, NULL, FALSE, timeout_ms - elapsed,
                                  QS_ALLINPUT);
    }
}

int main(void) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW klass = {0};
    klass.lpfnWndProc = test_window_proc;
    klass.hInstance = instance;
    klass.lpszClassName = L"GoldensPreviewServiceTest";
    if (!RegisterClassW(&klass)) return 1;
    HWND windows[3];
    for (int i = 0; i < 3; ++i)
        windows[i] = CreateWindowW(klass.lpszClassName, L"preview", WS_OVERLAPPED,
            10 + i * 20, 10, 320, 240, NULL, NULL, instance, NULL);
    if (!windows[0] || !windows[1] || !windows[2]) return 2;

    CaptureContext context = {0};
    context.first_started = CreateEventW(NULL, TRUE, FALSE, NULL);
    context.release_first = CreateEventW(NULL, TRUE, FALSE, NULL);
    GoldenPreviewService service = {0};
    int failed = !context.first_started || !context.release_first;
    if (!failed) failed = golden_preview_service_init(
        NULL, windows[0], PREVIEW_READY, controlled_capture, &context);
    if (!failed) failed = golden_preview_service_init(
        &service, NULL, PREVIEW_READY, controlled_capture, &context);
    if (!failed) failed = golden_preview_service_init(
        &service, windows[0], WM_USER, controlled_capture, &context);
    if (!failed) failed = golden_preview_service_init(
        &service, windows[0], PREVIEW_READY, NULL, &context);
    if (!failed) failed = service.implementation != NULL;
    if (!failed) failed =
        !golden_preview_service_init(&service, windows[0], PREVIEW_READY,
                                     controlled_capture, &context);
    if (!failed) failed = golden_preview_service_init(
        &service, windows[0], PREVIEW_READY, controlled_capture, &context);
    if (!failed) failed = golden_preview_service_complete(
        &service, windows[0], 1, NULL) != GOLDEN_PREVIEW_COMPLETION_NONE;
    if (!failed) failed = golden_preview_service_request(&service, NULL, 1);
    if (!failed) failed = !golden_preview_service_request(&service, windows[0], 1);
    if (!failed) failed = WaitForSingleObject(context.first_started, 5000) != WAIT_OBJECT_0;
    if (!failed) failed = !golden_preview_service_request(&service, windows[1], 2);
    if (!failed) failed = !golden_preview_service_request(&service, windows[2], 3);
    SetEvent(context.release_first);

    GoldenImage image = {0};
    if (!failed) failed = !wait_for_preview_message(5000);
    if (!failed) failed = golden_preview_service_complete(
        &service, windows[2], 3, &image) != GOLDEN_PREVIEW_COMPLETION_STALE;
    if (!failed) failed = !wait_for_preview_message(5000);
    if (!failed) failed = golden_preview_service_complete(
        &service, windows[2], 3, &image) != GOLDEN_PREVIEW_COMPLETION_ACCEPTED;
    if (!failed) failed = context.calls != 2 || context.targets[0] != windows[0] ||
                          context.targets[1] != windows[2] ||
                          context.thread_ids[0] != context.thread_ids[1] ||
                          !image.pixels || !image.width || !image.height;

    if (!failed) failed = !golden_preview_service_request(&service, windows[2], 4);
    if (!failed) failed = !wait_for_preview_message(5000);
    if (!failed) failed = golden_preview_service_complete(
        &service, windows[2], 4, &image) != GOLDEN_PREVIEW_COMPLETION_ACCEPTED;
    if (!failed) failed = context.calls != 3 ||
                          context.thread_ids[2] != context.thread_ids[0] ||
                          context.bitmaps[2] != context.bitmaps[0];

    context.fail_next = TRUE;
    if (!failed) failed = !golden_preview_service_request(&service, windows[2], 5);
    if (!failed) failed = !wait_for_preview_message(5000);
    if (!failed) failed = golden_preview_service_complete(
        &service, windows[2], 5, &image) != GOLDEN_PREVIEW_COMPLETION_FAILED;
    GoldenImage current = golden_preview_service_current_image(&service);
    if (!failed) failed = current.pixels != image.pixels;
    golden_preview_service_clear(&service);
    current = golden_preview_service_current_image(&service);
    if (!failed) failed = current.pixels != NULL;
    if (!golden_preview_service_shutdown(&service, 5000)) failed = 1;
    if (service.implementation) failed = 1;

    ResetEvent(context.first_started);
    ResetEvent(context.release_first);
    context.calls = 0;
    context.fail_next = FALSE;
    if (!failed) failed = !golden_preview_service_init(
        &service, windows[0], PREVIEW_READY, controlled_capture, &context);
    if (!failed) failed = !golden_preview_service_request(&service, windows[0], 6);
    if (!failed) failed = WaitForSingleObject(
        context.first_started, 5000) != WAIT_OBJECT_0;
    if (!failed) failed = golden_preview_service_shutdown(&service, 0);
    if (!failed) failed = service.implementation == NULL;
    SetEvent(context.release_first);
    if (!golden_preview_service_shutdown(&service, 5000)) failed = 1;
    if (service.implementation) failed = 1;

    CloseHandle(context.first_started);
    CloseHandle(context.release_first);
    for (int i = 0; i < 3; ++i) DestroyWindow(windows[i]);
    UnregisterClassW(klass.lpszClassName, instance);
    if (failed) return 1;
    puts("All Goldens preview service tests passed.");
    return 0;
}

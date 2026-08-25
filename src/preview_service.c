#include "preview_service.h"

#include <stdlib.h>

typedef struct {
    CRITICAL_SECTION lock;
    HANDLE request_event;
    HANDLE completion_event;
    HANDLE stop_event;
    HANDLE thread;
    HWND notification_window;
    UINT notification_message;
    GoldenPreviewCaptureOperation capture;
    void *capture_context;
    HWND requested_target;
    LONG requested_generation;
    BOOL request_pending;
    HWND completed_target;
    LONG completed_generation;
    int completed_surface;
    BOOL completed_success;
    BOOL completion_pending;
    int front_surface;
    int last_capture_surface;
    GoldenPreviewSurface surfaces[2];
} PreviewServiceImplementation;

static DWORD finish_preview_thread(PreviewServiceImplementation *service,
                                   DWORD result) {
    golden_preview_surface_release(&service->surfaces[0]);
    golden_preview_surface_release(&service->surfaces[1]);
    return result;
}

static DWORD WINAPI preview_thread(void *opaque) {
    PreviewServiceImplementation *service =
        (PreviewServiceImplementation *)opaque;
    HANDLE request_waits[] = {service->stop_event, service->request_event};
    HANDLE completion_waits[] = {service->stop_event,
                                 service->completion_event};
    for (;;) {
        DWORD wait = WaitForMultipleObjects(2, request_waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) return finish_preview_thread(service, 0);
        if (wait != WAIT_OBJECT_0 + 1) return finish_preview_thread(service, 1);

        EnterCriticalSection(&service->lock);
        if (!service->request_pending) {
            LeaveCriticalSection(&service->lock);
            continue;
        }
        HWND target = service->requested_target;
        LONG generation = service->requested_generation;
        service->request_pending = FALSE;
        int surface_index = (service->last_capture_surface + 1) % 2;
        if (surface_index == service->front_surface) surface_index = 1 - surface_index;
        service->last_capture_surface = surface_index;
        LeaveCriticalSection(&service->lock);

        BOOL captured = service->capture(
            target, &service->surfaces[surface_index], service->capture_context);
        if (WaitForSingleObject(service->stop_event, 0) == WAIT_OBJECT_0)
            return finish_preview_thread(service, 0);

        EnterCriticalSection(&service->lock);
        service->completed_target = target;
        service->completed_generation = generation;
        service->completed_surface = surface_index;
        service->completed_success = captured;
        service->completion_pending = TRUE;
        LeaveCriticalSection(&service->lock);
        if (!PostMessageW(service->notification_window,
                          service->notification_message, 0, 0))
            return finish_preview_thread(service, 0);

        wait = WaitForMultipleObjects(2, completion_waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) return finish_preview_thread(service, 0);
        if (wait != WAIT_OBJECT_0 + 1) return finish_preview_thread(service, 1);
    }
}

static void destroy_unstarted_service(PreviewServiceImplementation *service) {
    if (!service) return;
    if (service->request_event) CloseHandle(service->request_event);
    if (service->completion_event) CloseHandle(service->completion_event);
    if (service->stop_event) CloseHandle(service->stop_event);
    DeleteCriticalSection(&service->lock);
    free(service);
}

BOOL golden_preview_service_init(GoldenPreviewService *service,
                                 HWND notification_window,
                                 UINT notification_message,
                                 GoldenPreviewCaptureOperation capture,
                                 void *context) {
    if (!service || service->implementation || !notification_window ||
        !IsWindow(notification_window) || notification_message < WM_APP ||
        !capture) return FALSE;
    PreviewServiceImplementation *implementation =
        (PreviewServiceImplementation *)calloc(1, sizeof(*implementation));
    if (!implementation || !InitializeCriticalSectionEx(
            &implementation->lock, 0, 0)) {
        free(implementation);
        return FALSE;
    }
    implementation->front_surface = -1;
    implementation->last_capture_surface = -1;
    implementation->notification_window = notification_window;
    implementation->notification_message = notification_message;
    implementation->capture = capture;
    implementation->capture_context = context;
    implementation->request_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    implementation->completion_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    implementation->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!implementation->request_event || !implementation->completion_event ||
        !implementation->stop_event) {
        destroy_unstarted_service(implementation);
        return FALSE;
    }
    implementation->thread = CreateThread(
        NULL, 0, preview_thread, implementation, 0, NULL);
    if (!implementation->thread) {
        destroy_unstarted_service(implementation);
        return FALSE;
    }
    service->implementation = implementation;
    return TRUE;
}

BOOL golden_preview_service_request(GoldenPreviewService *service,
                                    HWND target, LONG generation) {
    PreviewServiceImplementation *implementation = service ?
        (PreviewServiceImplementation *)service->implementation : NULL;
    if (!implementation || !target || !IsWindow(target)) return FALSE;
    EnterCriticalSection(&implementation->lock);
    implementation->requested_target = target;
    implementation->requested_generation = generation;
    implementation->request_pending = TRUE;
    LeaveCriticalSection(&implementation->lock);
    return SetEvent(implementation->request_event);
}

GoldenPreviewCompletion golden_preview_service_complete(
    GoldenPreviewService *service, HWND expected_target,
    LONG expected_generation, GoldenImage *image) {
    PreviewServiceImplementation *implementation = service ?
        (PreviewServiceImplementation *)service->implementation : NULL;
    if (!implementation) return GOLDEN_PREVIEW_COMPLETION_NONE;
    EnterCriticalSection(&implementation->lock);
    if (!implementation->completion_pending) {
        LeaveCriticalSection(&implementation->lock);
        return GOLDEN_PREVIEW_COMPLETION_NONE;
    }
    GoldenPreviewCompletion completion;
    if (implementation->completed_target != expected_target ||
        implementation->completed_generation != expected_generation)
        completion = GOLDEN_PREVIEW_COMPLETION_STALE;
    else if (!implementation->completed_success)
        completion = GOLDEN_PREVIEW_COMPLETION_FAILED;
    else {
        implementation->front_surface = implementation->completed_surface;
        completion = GOLDEN_PREVIEW_COMPLETION_ACCEPTED;
    }
    if (image) {
        *image = implementation->front_surface >= 0 ?
            golden_preview_surface_image(
                &implementation->surfaces[implementation->front_surface]) :
            (GoldenImage){0};
    }
    implementation->completion_pending = FALSE;
    LeaveCriticalSection(&implementation->lock);
    SetEvent(implementation->completion_event);
    return completion;
}

GoldenImage golden_preview_service_current_image(
    const GoldenPreviewService *service) {
    PreviewServiceImplementation *implementation = service ?
        (PreviewServiceImplementation *)service->implementation : NULL;
    if (!implementation) return (GoldenImage){0};
    EnterCriticalSection(&implementation->lock);
    GoldenImage image = implementation->front_surface >= 0 ?
        golden_preview_surface_image(
            &implementation->surfaces[implementation->front_surface]) :
        (GoldenImage){0};
    LeaveCriticalSection(&implementation->lock);
    return image;
}

void golden_preview_service_clear(GoldenPreviewService *service) {
    PreviewServiceImplementation *implementation = service ?
        (PreviewServiceImplementation *)service->implementation : NULL;
    if (!implementation) return;
    EnterCriticalSection(&implementation->lock);
    implementation->front_surface = -1;
    LeaveCriticalSection(&implementation->lock);
}

BOOL golden_preview_service_shutdown(GoldenPreviewService *service,
                                     DWORD timeout_ms) {
    PreviewServiceImplementation *implementation = service ?
        (PreviewServiceImplementation *)service->implementation : NULL;
    if (!implementation) return TRUE;
    SetEvent(implementation->stop_event);
    DWORD wait = WaitForSingleObject(implementation->thread, timeout_ms);
    if (wait != WAIT_OBJECT_0) return FALSE;
    CloseHandle(implementation->thread);
    CloseHandle(implementation->request_event);
    CloseHandle(implementation->completion_event);
    CloseHandle(implementation->stop_event);
    DeleteCriticalSection(&implementation->lock);
    free(implementation);
    service->implementation = NULL;
    return TRUE;
}

#ifndef GOLDENS_PREVIEW_SERVICE_H
#define GOLDENS_PREVIEW_SERVICE_H

#include "preview_capture.h"

typedef BOOL (*GoldenPreviewCaptureOperation)(HWND target,
                                              GoldenPreviewSurface *surface,
                                              void *context);

typedef struct {
    void *implementation;
} GoldenPreviewService;

typedef enum {
    GOLDEN_PREVIEW_COMPLETION_NONE,
    GOLDEN_PREVIEW_COMPLETION_STALE,
    GOLDEN_PREVIEW_COMPLETION_FAILED,
    GOLDEN_PREVIEW_COMPLETION_ACCEPTED
} GoldenPreviewCompletion;

BOOL golden_preview_service_init(GoldenPreviewService *service,
                                 HWND notification_window,
                                 UINT notification_message,
                                 GoldenPreviewCaptureOperation capture,
                                 void *context);
BOOL golden_preview_service_request(GoldenPreviewService *service,
                                    HWND target, LONG generation);
GoldenPreviewCompletion golden_preview_service_complete(
    GoldenPreviewService *service, HWND expected_target,
    LONG expected_generation, GoldenImage *image);
GoldenImage golden_preview_service_current_image(
    const GoldenPreviewService *service);
void golden_preview_service_clear(GoldenPreviewService *service);
BOOL golden_preview_service_shutdown(GoldenPreviewService *service,
                                     DWORD timeout_ms);

#endif

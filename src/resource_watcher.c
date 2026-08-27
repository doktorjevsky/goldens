#include "resource_watcher.h"

#include <stdlib.h>

typedef struct {
    HANDLE change_notification;
    HANDLE stop_event;
    HANDLE thread;
    HWND notification_window;
    UINT notification_message;
    volatile LONG change_generation;
    volatile LONG notification_pending;
} ResourceWatcherImplementation;

static BOOL post_resource_change(ResourceWatcherImplementation *watcher) {
    InterlockedIncrement(&watcher->change_generation);
    if (InterlockedCompareExchange(
            &watcher->notification_pending, 1, 0) != 0)
        return TRUE;
    if (PostMessageW(watcher->notification_window,
                     watcher->notification_message, (WPARAM)FALSE, 0))
        return TRUE;
    InterlockedExchange(&watcher->notification_pending, 0);
    return FALSE;
}

static DWORD WINAPI resource_watcher_thread(void *opaque) {
    ResourceWatcherImplementation *watcher =
        (ResourceWatcherImplementation *)opaque;
    HANDLE waits[] = {watcher->stop_event, watcher->change_notification};
    for (;;) {
        DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) return 0;
        if (wait != WAIT_OBJECT_0 + 1 ||
            !FindNextChangeNotification(watcher->change_notification)) {
            PostMessageW(watcher->notification_window,
                         watcher->notification_message, (WPARAM)TRUE, 0);
            return 1;
        }
        /* Re-arm before notifying the UI. Changes that happen during or after
           the reconciliation therefore leave this handle signaled again. */
        if (!post_resource_change(watcher)) return 0;
    }
}

static void destroy_unstarted_watcher(ResourceWatcherImplementation *watcher) {
    if (!watcher) return;
    if (watcher->change_notification != INVALID_HANDLE_VALUE)
        FindCloseChangeNotification(watcher->change_notification);
    if (watcher->stop_event) CloseHandle(watcher->stop_event);
    free(watcher);
}

BOOL golden_resource_watcher_start(GoldenResourceWatcher *watcher,
                                   const wchar_t *directory,
                                   HWND notification_window,
                                   UINT notification_message) {
    if (!watcher || watcher->implementation || !directory || !directory[0] ||
        !notification_window || !IsWindow(notification_window) ||
        notification_message < WM_APP)
        return FALSE;
    ResourceWatcherImplementation *implementation =
        (ResourceWatcherImplementation *)calloc(1, sizeof(*implementation));
    if (!implementation) return FALSE;
    implementation->change_notification = INVALID_HANDLE_VALUE;
    implementation->notification_window = notification_window;
    implementation->notification_message = notification_message;
    implementation->change_notification = FindFirstChangeNotificationW(
        directory, TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
    implementation->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (implementation->change_notification == INVALID_HANDLE_VALUE ||
        !implementation->stop_event) {
        destroy_unstarted_watcher(implementation);
        return FALSE;
    }
    implementation->thread = CreateThread(
        NULL, 0, resource_watcher_thread, implementation, 0, NULL);
    if (!implementation->thread) {
        destroy_unstarted_watcher(implementation);
        return FALSE;
    }
    watcher->implementation = implementation;
    return TRUE;
}

LONG golden_resource_watcher_generation(const GoldenResourceWatcher *watcher) {
    ResourceWatcherImplementation *implementation = watcher ?
        (ResourceWatcherImplementation *)watcher->implementation : NULL;
    return implementation ? InterlockedCompareExchange(
        &implementation->change_generation, 0, 0) : 0;
}

void golden_resource_watcher_acknowledge(GoldenResourceWatcher *watcher) {
    ResourceWatcherImplementation *implementation = watcher ?
        (ResourceWatcherImplementation *)watcher->implementation : NULL;
    if (implementation)
        InterlockedExchange(&implementation->notification_pending, 0);
}

BOOL golden_resource_watcher_stop(GoldenResourceWatcher *watcher,
                                  DWORD timeout_ms) {
    ResourceWatcherImplementation *implementation = watcher ?
        (ResourceWatcherImplementation *)watcher->implementation : NULL;
    if (!implementation) return TRUE;
    SetEvent(implementation->stop_event);
    if (WaitForSingleObject(implementation->thread, timeout_ms) != WAIT_OBJECT_0)
        return FALSE;
    CloseHandle(implementation->thread);
    FindCloseChangeNotification(implementation->change_notification);
    CloseHandle(implementation->stop_event);
    free(implementation);
    watcher->implementation = NULL;
    return TRUE;
}

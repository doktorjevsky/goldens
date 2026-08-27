#ifndef GOLDENS_RESOURCE_WATCHER_H
#define GOLDENS_RESOURCE_WATCHER_H

#include <windows.h>

typedef struct {
    void *implementation;
} GoldenResourceWatcher;

BOOL golden_resource_watcher_start(GoldenResourceWatcher *watcher,
                                   const wchar_t *directory,
                                   HWND notification_window,
                                   UINT notification_message);
LONG golden_resource_watcher_generation(const GoldenResourceWatcher *watcher);
void golden_resource_watcher_acknowledge(GoldenResourceWatcher *watcher);
BOOL golden_resource_watcher_stop(GoldenResourceWatcher *watcher,
                                  DWORD timeout_ms);

#endif

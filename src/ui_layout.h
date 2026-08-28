#ifndef GOLDENS_UI_LAYOUT_H
#define GOLDENS_UI_LAYOUT_H

#include <windows.h>

#define GOLDEN_RESOURCE_PANE_MIN 130
#define GOLDEN_RESOURCE_PANE_DEFAULT GOLDEN_RESOURCE_PANE_MIN
#define GOLDEN_PANE_COLLAPSE_OVERSHOOT 48
#define GOLDEN_SPLITTER_WIDTH 10
#define GOLDEN_COLLAPSED_SPLITTER_WIDTH 16
#define GOLDEN_TOOL_BUTTON_COUNT 3
#define GOLDEN_VIEW_BUTTON_COUNT 3

typedef struct {
    RECT resource_tree;
    RECT left_splitter;
    RECT tool_buttons[GOLDEN_TOOL_BUTTON_COUNT];
    RECT context_label;
    RECT editor;
    RECT view_buttons[GOLDEN_VIEW_BUTTON_COUNT];
    RECT status;
} GoldenUiLayout;

GoldenUiLayout golden_compute_ui_layout(int width, int height, UINT dpi,
                                        int preferred_left,
                                        BOOL left_collapsed);
BOOL golden_pane_should_collapse(int raw_width, int minimum_width,
                                 BOOL currently_collapsed);
int golden_scale_ui(int value, UINT dpi);

#endif

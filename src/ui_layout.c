#include "ui_layout.h"

int golden_scale_ui(int value, UINT dpi) {
    return MulDiv(value, dpi ? (int)dpi : 96, 96);
}

BOOL golden_pane_should_collapse(int raw_width, int minimum_width,
                                 BOOL currently_collapsed) {
    if (currently_collapsed) return raw_width < minimum_width;
    return raw_width <= minimum_width - GOLDEN_PANE_COLLAPSE_OVERSHOOT;
}

static RECT make_rect(int x, int y, int width, int height) {
    RECT result = {x, y, x + max(1, width), y + max(1, height)};
    return result;
}

GoldenUiLayout golden_compute_ui_layout(int width, int height, UINT dpi,
                                        int preferred_left,
                                        BOOL left_collapsed) {
    GoldenUiLayout layout = {0};
    int top = golden_scale_ui(34, dpi);
    int status_height = golden_scale_ui(24, dpi);
    int gap = golden_scale_ui(2, dpi);
    int left_splitter = golden_scale_ui(left_collapsed ?
        GOLDEN_COLLAPSED_SPLITTER_WIDTH : GOLDEN_SPLITTER_WIDTH, dpi);
    int middle_min = golden_scale_ui(286, dpi);
    int left_min = golden_scale_ui(GOLDEN_RESOURCE_PANE_MIN, dpi);
    int left = left_collapsed ? 0 : golden_scale_ui(
        preferred_left > 0 ? preferred_left : GOLDEN_RESOURCE_PANE_DEFAULT, dpi);
    left = left_collapsed ? 0 : max(left_min, min(golden_scale_ui(520, dpi), left));
    left = min(left, max(0, width - left_splitter - gap - middle_min));

    int left_splitter_x = left;
    int editor_x = left_splitter_x + left_splitter + gap;
    int middle = max(1, width - editor_x);
    int content_bottom = max(top + 1, height - status_height);
    int content_height = content_bottom - top;
    layout.resource_tree = left_collapsed ? (RECT){0, 0, 0, content_bottom} :
                           make_rect(0, 0, left, content_bottom);
    layout.left_splitter = make_rect(left_splitter_x, 0, left_splitter, content_bottom);
    layout.editor = make_rect(editor_x, top, middle, content_height);
    layout.status = make_rect(0, content_bottom, width, status_height);

    int tool_x = editor_x + golden_scale_ui(4, dpi);
    int tool_size = golden_scale_ui(26, dpi);
    for (int i = 0; i < GOLDEN_TOOL_BUTTON_COUNT; ++i) {
        layout.tool_buttons[i] = make_rect(tool_x, golden_scale_ui(4, dpi),
            tool_size, tool_size);
        tool_x = layout.tool_buttons[i].right + golden_scale_ui(3, dpi);
    }

    const int view_widths[GOLDEN_VIEW_BUTTON_COUNT] = {38, 26, 26};
    int view_total = golden_scale_ui(102, dpi);
    int view_x = editor_x + middle - view_total;
    layout.context_label = make_rect(tool_x + golden_scale_ui(1, dpi), 0,
        max(1, view_x - tool_x - golden_scale_ui(5, dpi)), top);
    for (int i = 0; i < GOLDEN_VIEW_BUTTON_COUNT; ++i) {
        layout.view_buttons[i] = make_rect(view_x, golden_scale_ui(5, dpi),
            golden_scale_ui(view_widths[i], dpi), golden_scale_ui(24, dpi));
        view_x = layout.view_buttons[i].right + golden_scale_ui(3, dpi);
    }

    return layout;
}

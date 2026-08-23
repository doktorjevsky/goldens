#include "ui_layout.h"

#include <stdint.h>

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
                                        int preferred_left, int preferred_right,
                                        BOOL left_collapsed, BOOL right_collapsed) {
    GoldenUiLayout layout = {0};
    int top = golden_scale_ui(40, dpi);
    int status_height = golden_scale_ui(24, dpi);
    int gap = golden_scale_ui(2, dpi);
    int left_splitter = golden_scale_ui(left_collapsed ?
        GOLDEN_COLLAPSED_SPLITTER_WIDTH : GOLDEN_SPLITTER_WIDTH, dpi);
    int right_splitter = golden_scale_ui(right_collapsed ?
        GOLDEN_COLLAPSED_SPLITTER_WIDTH : GOLDEN_SPLITTER_WIDTH, dpi);
    int palette = golden_scale_ui(82, dpi);
    int middle_min = golden_scale_ui(286, dpi);
    int left_min = golden_scale_ui(GOLDEN_RESOURCE_PANE_MIN, dpi);
    int right_min = golden_scale_ui(GOLDEN_WINDOWS_PANE_MIN, dpi);
    int left = left_collapsed ? 0 : golden_scale_ui(
        preferred_left > 0 ? preferred_left : 270, dpi);
    int right = right_collapsed ? 0 : golden_scale_ui(
        preferred_right > 0 ? preferred_right : 320, dpi);
    left = left_collapsed ? 0 : max(left_min, min(golden_scale_ui(520, dpi), left));
    right = right_collapsed ? 0 : max(right_min, min(golden_scale_ui(560, dpi), right));

    int maximum_sides = max(0, width - palette - left_splitter -
                            right_splitter - gap - middle_min);
    if (left + right > maximum_sides) {
        int minimum_sum = (left_collapsed ? 0 : left_min) +
                          (right_collapsed ? 0 : right_min);
        if (maximum_sides >= minimum_sum) {
            int extra = maximum_sides - minimum_sum;
            int desired_extra = max(1, left + right - minimum_sum);
            int left_extra = left_collapsed ? 0 :
                (int)((int64_t)extra * max(0, left - left_min) / desired_extra);
            left = left_collapsed ? 0 : left_min + left_extra;
            right = right_collapsed ? 0 : maximum_sides - left;
        } else if (!left_collapsed && !right_collapsed) {
            left = maximum_sides * 38 / 100;
            right = maximum_sides - left;
        } else if (!left_collapsed) left = maximum_sides;
        else right = maximum_sides;
    }

    int left_splitter_x = left;
    int palette_x = left_splitter_x + left_splitter;
    int editor_x = palette_x + palette + gap;
    int middle = max(1, width - editor_x - right_splitter - right);
    int right_splitter_x = editor_x + middle;
    int windows_x = right_splitter_x + right_splitter;
    int content_bottom = max(top + 1, height - status_height);
    int content_height = content_bottom - top;
    layout.resource_tree = left_collapsed ? (RECT){0, 0, 0, content_bottom} :
                           make_rect(0, 0, left, content_bottom);
    layout.left_splitter = make_rect(left_splitter_x, 0, left_splitter, content_bottom);
    layout.editor = make_rect(editor_x, top, middle, content_height);
    layout.right_splitter = make_rect(right_splitter_x, 0, right_splitter, content_bottom);
    layout.window_tree = right_collapsed ? (RECT){windows_x, top, windows_x, content_bottom} :
                         make_rect(windows_x, top, right, content_height);
    layout.status = make_rect(0, content_bottom, width, status_height);

    int tool_x = palette_x + golden_scale_ui(7, dpi);
    int tool_width = palette - golden_scale_ui(12, dpi);
    for (int i = 0; i < 4; ++i)
        layout.tool_buttons[i] = make_rect(tool_x,
            golden_scale_ui(8 + i * 43, dpi), tool_width, golden_scale_ui(36, dpi));

    const int view_widths[4] = {46, 34, 34, 58};
    int view_total = golden_scale_ui(190, dpi);
    int view_x = editor_x + middle - view_total;
    layout.context_label = make_rect(editor_x, 0,
        max(golden_scale_ui(72, dpi), view_x - editor_x - golden_scale_ui(4, dpi)), top);
    for (int i = 0; i < 4; ++i) {
        layout.view_buttons[i] = make_rect(view_x, golden_scale_ui(6, dpi),
            golden_scale_ui(view_widths[i], dpi), golden_scale_ui(28, dpi));
        view_x = layout.view_buttons[i].right + golden_scale_ui(4, dpi);
    }

    const int window_widths[2] = {74, 82};
    if (!right_collapsed) {
        int window_x = windows_x + right - golden_scale_ui(
            160 + GOLDEN_WINDOW_BUTTON_RIGHT_INSET, dpi);
        for (int i = 0; i < 2; ++i) {
            layout.window_buttons[i] = make_rect(window_x, golden_scale_ui(6, dpi),
                golden_scale_ui(window_widths[i], dpi), golden_scale_ui(28, dpi));
            window_x = layout.window_buttons[i].right + golden_scale_ui(4, dpi);
        }
    }
    return layout;
}

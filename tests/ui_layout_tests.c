#include <windows.h>

#include <stdio.h>

#include "../src/ui_layout.h"

static int overlaps_horizontally(RECT left, RECT right) {
    return left.right > right.left && right.right > left.left;
}

static int check_layout(int width, int height, UINT dpi, int preferred_left,
                        int preferred_right, BOOL left_collapsed, BOOL right_collapsed) {
    GoldenUiLayout layout = golden_compute_ui_layout(width, height, dpi,
        preferred_left, preferred_right, left_collapsed, right_collapsed);
    if (layout.resource_tree.left < 0 || layout.status.left < 0 ||
        layout.status.right > width || layout.status.bottom > height) return 1;
    if ((!left_collapsed && overlaps_horizontally(layout.resource_tree, layout.editor)) ||
        overlaps_horizontally(layout.editor, layout.window_tree)) return 1;
    if ((left_collapsed && layout.resource_tree.right != layout.resource_tree.left) ||
        (right_collapsed && layout.window_tree.right != layout.window_tree.left)) return 1;
    int left_splitter_width = layout.left_splitter.right - layout.left_splitter.left;
    int right_splitter_width = layout.right_splitter.right - layout.right_splitter.left;
    if (left_splitter_width != golden_scale_ui(left_collapsed ?
            GOLDEN_COLLAPSED_SPLITTER_WIDTH : GOLDEN_SPLITTER_WIDTH, dpi) ||
        right_splitter_width != golden_scale_ui(right_collapsed ?
            GOLDEN_COLLAPSED_SPLITTER_WIDTH : GOLDEN_SPLITTER_WIDTH, dpi)) return 1;
    if ((left_collapsed && layout.left_splitter.left != 0) ||
        (right_collapsed && layout.right_splitter.right != width)) return 1;
    if (layout.context_label.right > layout.view_buttons[0].left ||
        layout.context_label.left <
            layout.tool_buttons[GOLDEN_TOOL_BUTTON_COUNT - 1].right) return 1;
    for (int i = 0; i < GOLDEN_VIEW_BUTTON_COUNT; ++i) {
        if (layout.view_buttons[i].left < layout.editor.left ||
            layout.view_buttons[i].right > layout.editor.right) return 1;
        if (i && layout.view_buttons[i - 1].right > layout.view_buttons[i].left) return 1;
    }
    for (int i = 0; i < GOLDEN_TOOL_BUTTON_COUNT; ++i) {
        if (layout.tool_buttons[i].left < layout.editor.left ||
            layout.tool_buttons[i].right > layout.editor.right ||
            layout.tool_buttons[i].bottom > layout.editor.top) return 1;
        if (layout.tool_buttons[i].right - layout.tool_buttons[i].left !=
                golden_scale_ui(26, dpi) ||
            layout.tool_buttons[i].bottom - layout.tool_buttons[i].top !=
                golden_scale_ui(26, dpi)) return 1;
        if (i && (layout.tool_buttons[i - 1].right > layout.tool_buttons[i].left ||
                  layout.tool_buttons[i - 1].top != layout.tool_buttons[i].top)) return 1;
    }
    for (int i = 0; !right_collapsed && i < 2; ++i) {
        if (layout.window_buttons[i].left < layout.window_tree.left ||
            layout.window_buttons[i].right > layout.window_tree.right) return 1;
        if (i && layout.window_buttons[i - 1].right > layout.window_buttons[i].left) return 1;
    }
    if (!right_collapsed && layout.window_tree.right - layout.window_buttons[1].right !=
        golden_scale_ui(GOLDEN_WINDOW_BUTTON_RIGHT_INSET, dpi)) return 1;
    return layout.editor.right > width || layout.window_tree.right > width;
}

int main(void) {
    int failed = 0;
    GoldenUiLayout defaults = golden_compute_ui_layout(1600, 900, 96,
        0, 0, FALSE, FALSE);
    failed |= defaults.resource_tree.right - defaults.resource_tree.left !=
              GOLDEN_RESOURCE_PANE_DEFAULT;
    failed |= defaults.window_tree.right - defaults.window_tree.left !=
              GOLDEN_WINDOWS_PANE_DEFAULT;
    failed |= check_layout(800, 480, 96, 270, 320, FALSE, FALSE);
    failed |= check_layout(1000, 650, 96, 420, 260, FALSE, FALSE);
    failed |= check_layout(1280, 800, 96, 180, 500, FALSE, FALSE);
    failed |= check_layout(1920, 1080, 96, 500, 560, FALSE, FALSE);
    failed |= check_layout(800, 480, 96, 270, 320, TRUE, FALSE);
    failed |= check_layout(800, 480, 96, 270, 320, FALSE, TRUE);
    failed |= check_layout(800, 480, 96, 270, 320, TRUE, TRUE);
    failed |= check_layout(1200, 720, 144, 270, 320, FALSE, FALSE);
    failed |= check_layout(1600, 900, 192, 400, 250, FALSE, TRUE);
    failed |= golden_pane_should_collapse(GOLDEN_RESOURCE_PANE_MIN,
                                           GOLDEN_RESOURCE_PANE_MIN, FALSE);
    failed |= golden_pane_should_collapse(
        GOLDEN_RESOURCE_PANE_MIN - GOLDEN_PANE_COLLAPSE_OVERSHOOT + 1,
        GOLDEN_RESOURCE_PANE_MIN, FALSE);
    failed |= !golden_pane_should_collapse(
        GOLDEN_RESOURCE_PANE_MIN - GOLDEN_PANE_COLLAPSE_OVERSHOOT,
        GOLDEN_RESOURCE_PANE_MIN, FALSE);
    failed |= !golden_pane_should_collapse(GOLDEN_WINDOWS_PANE_MIN - 1,
                                            GOLDEN_WINDOWS_PANE_MIN, TRUE);
    failed |= golden_pane_should_collapse(GOLDEN_WINDOWS_PANE_MIN,
                                           GOLDEN_WINDOWS_PANE_MIN, TRUE);
    if (failed) return 1;
    puts("All Goldens responsive layout tests passed.");
    return 0;
}

#include <windows.h>

#include <stdio.h>

#include "../src/ui_layout.h"

static int overlaps_horizontally(RECT left, RECT right) {
    return left.right > right.left && right.right > left.left;
}

static int check_layout(int width, int height, UINT dpi, int preferred_left,
                        BOOL left_collapsed) {
    GoldenUiLayout layout = golden_compute_ui_layout(
        width, height, dpi, preferred_left, left_collapsed);
    if (layout.resource_tree.left < 0 || layout.status.left < 0 ||
        layout.status.right > width || layout.status.bottom > height ||
        layout.editor.right > width) return 1;
    if (!left_collapsed &&
        overlaps_horizontally(layout.resource_tree, layout.editor)) return 1;
    if (left_collapsed &&
        layout.resource_tree.right != layout.resource_tree.left) return 1;
    int splitter_width =
        layout.left_splitter.right - layout.left_splitter.left;
    if (splitter_width != golden_scale_ui(left_collapsed ?
            GOLDEN_COLLAPSED_SPLITTER_WIDTH : GOLDEN_SPLITTER_WIDTH, dpi) ||
        (left_collapsed && layout.left_splitter.left != 0)) return 1;
    if (layout.context_label.right > layout.view_buttons[0].left ||
        layout.context_label.left <
            layout.tool_buttons[GOLDEN_TOOL_BUTTON_COUNT - 1].right) return 1;
    for (int i = 0; i < GOLDEN_VIEW_BUTTON_COUNT; ++i) {
        if (layout.view_buttons[i].left < layout.editor.left ||
            layout.view_buttons[i].right > layout.editor.right ||
            (i && layout.view_buttons[i - 1].right >
                  layout.view_buttons[i].left)) return 1;
    }
    for (int i = 0; i < GOLDEN_TOOL_BUTTON_COUNT; ++i) {
        if (layout.tool_buttons[i].left < layout.editor.left ||
            layout.tool_buttons[i].right > layout.editor.right ||
            layout.tool_buttons[i].bottom > layout.editor.top ||
            layout.tool_buttons[i].right - layout.tool_buttons[i].left !=
                golden_scale_ui(26, dpi) ||
            layout.tool_buttons[i].bottom - layout.tool_buttons[i].top !=
                golden_scale_ui(26, dpi) ||
            (i && layout.tool_buttons[i - 1].right >
                  layout.tool_buttons[i].left)) return 1;
    }
    return 0;
}

int main(void) {
    int failed = 0;
    GoldenUiLayout defaults = golden_compute_ui_layout(
        1600, 900, 96, 0, FALSE);
    failed |= defaults.resource_tree.right - defaults.resource_tree.left !=
              GOLDEN_RESOURCE_PANE_DEFAULT;
    failed |= defaults.editor.right != 1600;
    failed |= check_layout(1600, 900, 96, 0, FALSE);
    failed |= check_layout(1024, 640, 96, 300, FALSE);
    failed |= check_layout(800, 480, 144, 520, FALSE);
    failed |= check_layout(800, 480, 192, 520, TRUE);
    failed |= golden_scale_ui(26, 144) != 39;
    failed |= !golden_pane_should_collapse(
        GOLDEN_RESOURCE_PANE_MIN - GOLDEN_PANE_COLLAPSE_OVERSHOOT,
        GOLDEN_RESOURCE_PANE_MIN, FALSE);
    failed |= golden_pane_should_collapse(
        GOLDEN_RESOURCE_PANE_MIN, GOLDEN_RESOURCE_PANE_MIN, TRUE);
    if (failed) {
        fprintf(stderr, "Goldens two-pane layout test failed\n");
        return 1;
    }
    puts("All Goldens responsive layout tests passed.");
    return 0;
}

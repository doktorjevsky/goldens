#include "model.h"

#include <string.h>
#include <wchar.h>
#include <stdio.h>

BOOL golden_name_exists(const Annotation *items, int count, const wchar_t *name, int except) {
    for (int i = 0; i < count; ++i)
        if (i != except && _wcsicmp(items[i].name, name) == 0) return TRUE;
    return FALSE;
}

void golden_make_unique_name(const Annotation *items, int count, wchar_t *out, size_t capacity) {
    int suffix = count + 1;
    do { _snwprintf(out, capacity, L"annotation_%d", suffix++); }
    while (golden_name_exists(items, count, out, -1));
    out[capacity - 1] = 0;
}

RECT golden_normalize_rect(POINT first, POINT second) {
    RECT result = {
        min(first.x, second.x), min(first.y, second.y),
        max(first.x, second.x), max(first.y, second.y)
    };
    return result;
}

RECT golden_clamp_rect(RECT rect, int image_width, int image_height) {
    int width = max(1L, rect.right - rect.left);
    int height = max(1L, rect.bottom - rect.top);
    width = min(width, image_width);
    height = min(height, image_height);
    rect.left = min((LONG)image_width - width, max(0L, rect.left));
    rect.top = min((LONG)image_height - height, max(0L, rect.top));
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return rect;
}

BOOL golden_set_click(Annotation *annotation, POINT image_point) {
    if (!annotation || !PtInRect(&annotation->boundary, image_point)) return FALSE;
    LONG width = max(1L, annotation->boundary.right - annotation->boundary.left);
    LONG height = max(1L, annotation->boundary.bottom - annotation->boundary.top);
    annotation->click_x = (double)(image_point.x - annotation->boundary.left) / width;
    annotation->click_y = (double)(image_point.y - annotation->boundary.top) / height;
    annotation->has_click = TRUE;
    return TRUE;
}

int golden_hit_annotation(const Annotation *items, int count, POINT image_point) {
    if (!items || count <= 0) return -1;
    for (int i = count - 1; i >= 0; --i)
        if (PtInRect(&items[i].boundary, image_point)) return i;
    return -1;
}

GoldenViewport golden_compute_viewport(int image_width, int image_height,
                                       int client_width, int client_height,
                                       int top_inset, double zoom,
                                       int pan_x, int pan_y) {
    GoldenViewport result = {{0, 0, 0, 0}, 1.0};
    if (image_width <= 0 || image_height <= 0) return result;
    if (zoom > 0.0) result.scale = min(8.0, max(0.05, zoom));
    else {
        double sx = (double)max(1, client_width - 30) / image_width;
        double sy = (double)max(1, client_height - top_inset - 20) / image_height;
        result.scale = min(1.0, min(sx, sy));
    }
    int width = max(1, (int)(image_width * result.scale + 0.5));
    int height = max(1, (int)(image_height * result.scale + 0.5));
    result.destination.left = (client_width - width) / 2 + pan_x;
    result.destination.top = top_inset + max(0, (client_height - top_inset - height) / 2) + pan_y;
    result.destination.right = result.destination.left + width;
    result.destination.bottom = result.destination.top + height;
    return result;
}

BOOL golden_view_to_image(const GoldenViewport *viewport, POINT client,
                          int image_width, int image_height, POINT *image) {
    if (!viewport || !image || viewport->scale <= 0.0 ||
        !PtInRect(&viewport->destination, client)) return FALSE;
    image->x = (LONG)((client.x - viewport->destination.left) / viewport->scale);
    image->y = (LONG)((client.y - viewport->destination.top) / viewport->scale);
    image->x = min((LONG)image_width - 1, max(0L, image->x));
    image->y = min((LONG)image_height - 1, max(0L, image->y));
    return TRUE;
}

BOOL golden_window_lists_equal(const GoldenWindowInfo *left, int left_count,
                               const GoldenWindowInfo *right, int right_count) {
    if (left_count != right_count) return FALSE;
    for (int i = 0; i < left_count; ++i) {
        if (left[i].id != right[i].id || wcscmp(left[i].app, right[i].app) ||
            wcscmp(left[i].title, right[i].title)) return FALSE;
    }
    return TRUE;
}

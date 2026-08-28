#ifndef GOLDENS_MODEL_H
#define GOLDENS_MODEL_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_ANNOTATIONS 256

typedef struct {
    wchar_t name[128];
    RECT boundary;
    BOOL has_click;
    double click_x;
    double click_y;
} Annotation;

typedef struct {
    RECT destination;
    double scale;
} GoldenViewport;

BOOL golden_name_exists(const Annotation *items, int count, const wchar_t *name, int except);
void golden_make_unique_name(const Annotation *items, int count, wchar_t *out, size_t capacity);
RECT golden_normalize_rect(POINT first, POINT second);
RECT golden_clamp_rect(RECT rect, int image_width, int image_height);
BOOL golden_set_click(Annotation *annotation, POINT image_point);
int golden_hit_annotation(const Annotation *items, int count, POINT image_point);
GoldenViewport golden_compute_viewport(int image_width, int image_height,
                                       int client_width, int client_height,
                                       int top_inset, double zoom,
                                       int pan_x, int pan_y);
POINT golden_zoom_anchor_pan(const GoldenViewport *current,
                             const GoldenViewport *centered_zoom,
                             POINT client_anchor);
BOOL golden_view_to_image(const GoldenViewport *viewport, POINT client,
                          int image_width, int image_height, POINT *image);
#endif

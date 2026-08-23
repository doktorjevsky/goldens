#include "../src/model.h"
#include "../src/document.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int failures;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fwprintf(stderr, L"FAIL %S:%d: %S\n", __FILE__, __LINE__, #expression); \
        failures++; \
    } \
} while (0)

static void test_annotation_names(void) {
    Annotation items[3] = {0};
    wcscpy(items[0].name, L"button");
    wcscpy(items[1].name, L"annotation_3");
    CHECK(golden_name_exists(items, 2, L"BUTTON", -1));
    CHECK(!golden_name_exists(items, 2, L"button", 0));
    wchar_t name[128];
    golden_make_unique_name(items, 2, name, 128);
    CHECK(wcscmp(name, L"annotation_4") == 0);
}

static void test_rectangles_and_clicks(void) {
    POINT a = {80, 60}, b = {20, 10};
    RECT rect = golden_normalize_rect(a, b);
    CHECK(rect.left == 20 && rect.top == 10 && rect.right == 80 && rect.bottom == 60);
    RECT outside = {-20, 90, 40, 150};
    rect = golden_clamp_rect(outside, 100, 100);
    CHECK(rect.left == 0 && rect.top == 40 && rect.right == 60 && rect.bottom == 100);
    Annotation annotation = {0};
    annotation.boundary = (RECT){10, 20, 110, 70};
    CHECK(golden_set_click(&annotation, (POINT){60, 45}));
    CHECK(annotation.click_x == 0.5 && annotation.click_y == 0.5);
    CHECK(!golden_set_click(&annotation, (POINT){111, 45}));

    Annotation overlapping[2] = {0};
    overlapping[0].boundary = (RECT){0, 0, 80, 80};
    overlapping[1].boundary = (RECT){20, 20, 60, 60};
    CHECK(golden_hit_annotation(overlapping, 2, (POINT){10, 10}) == 0);
    CHECK(golden_hit_annotation(overlapping, 2, (POINT){30, 30}) == 1);
    CHECK(golden_hit_annotation(overlapping, 2, (POINT){90, 90}) == -1);
    CHECK(golden_hit_annotation(NULL, 2, (POINT){30, 30}) == -1);
}

static void test_viewport_fidelity(void) {
    GoldenViewport fit = golden_compute_viewport(1920, 1080, 960, 700, 30, 0.0, 0, 0);
    CHECK(fit.scale > 0.47 && fit.scale < 0.49);
    GoldenViewport one = golden_compute_viewport(1920, 1080, 960, 700, 30, 1.0, 0, 0);
    CHECK(one.scale == 1.0);
    CHECK(one.destination.right - one.destination.left == 1920);
    POINT image;
    POINT client = {one.destination.left + 321, one.destination.top + 222};
    CHECK(golden_view_to_image(&one, client, 1920, 1080, &image));
    CHECK(image.x == 321 && image.y == 222);
}

static void test_window_reconciliation(void) {
    GoldenWindowInfo before[2] = {
        {1, L"alpha.exe", L"First"}, {2, L"beta.exe", L"Second"}
    };
    GoldenWindowInfo same[2];
    memcpy(same, before, sizeof(before));
    CHECK(golden_window_lists_equal(before, 2, same, 2));
    same[1].title[0] = L'X';
    CHECK(!golden_window_lists_equal(before, 2, same, 2));
    CHECK(!golden_window_lists_equal(before, 2, before, 1));
}

static void test_document_round_trip(void) {
    Annotation source[2] = {0};
    wcscpy(source[0].name, L"Save \"button\"");
    source[0].boundary = (RECT){12, 34, 212, 84};
    source[0].has_click = TRUE;
    source[0].click_x = 0.25;
    source[0].click_y = 0.75;
    wcscpy(source[1].name, L"status\\label");
    source[1].boundary = (RECT){1, 2, 4, 6};
    size_t length = 0;
    char *json = golden_document_serialize_utf8(source, 2, &length);
    CHECK(json != NULL && length > 0);
    Annotation parsed[MAX_ANNOTATIONS] = {0};
    int count = MAX_ANNOTATIONS;
    CHECK(golden_document_parse_utf8(json, length, parsed, &count));
    CHECK(count == 2);
    CHECK(wcscmp(parsed[0].name, source[0].name) == 0);
    CHECK(EqualRect(&parsed[0].boundary, &source[0].boundary));
    CHECK(parsed[0].has_click && parsed[0].click_x == 0.25 && parsed[0].click_y == 0.75);
    CHECK(wcscmp(parsed[1].name, source[1].name) == 0);
    free(json);

    const char *invalid = "{\"annotations\":[{\"name\":\"x\",\"boundary\":{\"x\":0,\"y\":0,\"width\":0,\"height\":1}}]}";
    count = MAX_ANNOTATIONS;
    CHECK(!golden_document_parse_utf8(invalid, strlen(invalid), parsed, &count));
}

int main(void) {
    test_annotation_names();
    test_rectangles_and_clicks();
    test_viewport_fidelity();
    test_window_reconciliation();
    test_document_round_trip();
    if (failures) {
        fprintf(stderr, "%d model test(s) failed.\n", failures);
        return 1;
    }
    puts("All Goldens model tests passed.");
    return 0;
}

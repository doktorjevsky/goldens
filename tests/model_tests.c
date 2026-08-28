#include "../src/model.h"
#include "../src/document.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

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

    GoldenViewport current = golden_compute_viewport(
        1000, 800, 800, 600, 30, 0.5, 35, -20);
    POINT anchor = {615, 185};
    double anchored_image_x =
        (anchor.x - current.destination.left) / current.scale;
    double anchored_image_y =
        (anchor.y - current.destination.top) / current.scale;
    GoldenViewport centered_zoom = golden_compute_viewport(
        1000, 800, 800, 600, 30, 1.0, 0, 0);
    POINT pan = golden_zoom_anchor_pan(&current, &centered_zoom, anchor);
    GoldenViewport zoomed = golden_compute_viewport(
        1000, 800, 800, 600, 30, 1.0, pan.x, pan.y);
    double zoomed_client_x = zoomed.destination.left + anchored_image_x * zoomed.scale;
    double zoomed_client_y = zoomed.destination.top + anchored_image_y * zoomed.scale;
    CHECK(fabs(zoomed_client_x - anchor.x) <= 0.5);
    CHECK(fabs(zoomed_client_y - anchor.y) <= 0.5);
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

static BOOL parse_document(const char *json, Annotation *parsed, int *count) {
    return golden_document_parse_utf8(json, strlen(json), parsed, count);
}

static void test_document_special_names(void) {
    Annotation source[5] = {0};
    wcscpy(source[0].name, L"name");
    wcscpy(source[1].name, L"click");
    wcscpy(source[2].name, L"boundary");
    wcscpy(source[3].name, L"quoted \"name\" and \\ slash");
    source[4].name[0] = L'c';
    source[4].name[1] = 1;
    source[4].name[2] = L'x';
    for (int i = 0; i < 5; ++i)
        source[i].boundary = (RECT){i, i + 1, i + 3, i + 5};

    size_t length = 0;
    char *json = golden_document_serialize_utf8(source, 5, &length);
    CHECK(json != NULL);
    CHECK(json && strstr(json, "\\u0001") != NULL);
    Annotation parsed[MAX_ANNOTATIONS] = {0};
    int count = MAX_ANNOTATIONS;
    CHECK(json && golden_document_parse_utf8(json, length, parsed, &count));
    CHECK(count == 5);
    for (int i = 0; json && i < count && i < 5; ++i) {
        CHECK(wcscmp(parsed[i].name, source[i].name) == 0);
        CHECK(EqualRect(&parsed[i].boundary, &source[i].boundary));
    }
    free(json);
}

static void test_document_valid_variants(void) {
    const char *json =
        "{\"version\":1,\"annotations\":[{"
        "\"boundary\":{\"height\":4,\"future\":true,\"width\":3,\"y\":2,\"x\":1},"
        "\"future\":[null,{\"nested\":false}],"
        "\"click\":{\"y\":0.75,\"x\":2.5e-1},"
        "\"name\":\"A\\u00e9\\ud83d\\ude00\"}]}";
    Annotation parsed[2] = {0};
    int count = 2;
    CHECK(parse_document(json, parsed, &count));
    CHECK(count == 1);
    CHECK(parsed[0].name[0] == L'A');
    CHECK(parsed[0].name[1] == 0x00e9);
    CHECK(parsed[0].name[2] == 0xd83d);
    CHECK(parsed[0].name[3] == 0xde00);
    CHECK(parsed[0].name[4] == 0);
    CHECK(EqualRect(&parsed[0].boundary, &(RECT){1, 2, 4, 6}));
    CHECK(parsed[0].has_click);
    CHECK(parsed[0].click_x == 0.25 && parsed[0].click_y == 0.75);
}

static void check_invalid_document(const char *json) {
    Annotation parsed[4] = {0};
    int count = 4;
    CHECK(!parse_document(json, parsed, &count));
}

static void test_document_rejects_invalid_json(void) {
    check_invalid_document("");
    check_invalid_document("not json");
    check_invalid_document("{}");
    check_invalid_document("{\"annotations\":{}}");
    check_invalid_document("{\"annotations\":[]} trailing");
    check_invalid_document("{\"annotations\":[null]}");
    check_invalid_document("{\"annotations\":[{\"name\":\"x\"}]}");
    check_invalid_document("{\"annotations\":[{\"name\":\"x\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1},\"click\":{\"x\":0.5}}]}");
    check_invalid_document("{\"annotations\":[{\"name\":\"bad\\q\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}}]}");
    check_invalid_document("{\"annotations\":[{\"name\":\"bad\\ud800\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}}]}");
    check_invalid_document("{\"annotations\":[{\"name\":\"\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}}]}");
    check_invalid_document("{\"annotations\":[{\"name\":\"x\",\"name\":\"y\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}}]}");
    check_invalid_document("{\"annotations\":[{\"name\":\"x\",\"boundary\":{\"x\":0,\"x\":1,\"y\":0,\"width\":1,\"height\":1}}]}");
    check_invalid_document("{\"annotations\":[{\"name\":\"x\",\"boundary\":{\"x\":2147483647,\"y\":0,\"width\":1,\"height\":1}}]}");
    check_invalid_document("{\"annotations\":[{\"name\":\"x\",\"boundary\":{\"x\":0,\"y\":0,\"width\":2147483648,\"height\":1}}]}");
    check_invalid_document("{\"annotations\":[{\"name\":\"x\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1},\"click\":{\"x\":nan,\"y\":0.5}}]}");
    check_invalid_document("{\"annotations\":[{\"name\":\"x\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1},\"click\":{\"x\":1.1,\"y\":0.5}}]}");

    const char unescaped_control[] =
        "{\"annotations\":[{\"name\":\"bad\x01name\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}}]}";
    check_invalid_document(unescaped_control);
}

static void test_document_rejects_duplicates_and_truncation(void) {
    const char *duplicates =
        "{\"annotations\":["
        "{\"name\":\"Same\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}},"
        "{\"name\":\"same\",\"boundary\":{\"x\":1,\"y\":1,\"width\":1,\"height\":1}}]}";
    check_invalid_document(duplicates);

    const char *two =
        "{\"annotations\":["
        "{\"name\":\"one\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}},"
        "{\"name\":\"two\",\"boundary\":{\"x\":1,\"y\":1,\"width\":1,\"height\":1}}]}";
    Annotation parsed[1] = {0};
    int count = 1;
    CHECK(!parse_document(two, parsed, &count));
}

static void test_document_rejects_invalid_model(void) {
    Annotation annotation = {0};
    wcscpy(annotation.name, L"x");
    annotation.boundary = (RECT){0, 0, 1, 1};
    annotation.has_click = TRUE;
    annotation.click_x = NAN;
    annotation.click_y = 0.5;
    CHECK(golden_document_serialize_utf8(&annotation, 1, NULL) == NULL);
    CHECK(golden_document_serialize_utf8(NULL, 1, NULL) == NULL);
    CHECK(golden_document_serialize_utf8(&annotation, -1, NULL) == NULL);

    annotation.has_click = FALSE;
    annotation.boundary.right = annotation.boundary.left;
    CHECK(golden_document_serialize_utf8(&annotation, 1, NULL) == NULL);
    Annotation duplicates[2] = {0};
    wcscpy(duplicates[0].name, L"duplicate");
    wcscpy(duplicates[1].name, L"DUPLICATE");
    duplicates[0].boundary = duplicates[1].boundary = (RECT){0, 0, 1, 1};
    CHECK(golden_document_serialize_utf8(duplicates, 2, NULL) == NULL);

    for (size_t i = 0; i < _countof(annotation.name); ++i)
        annotation.name[i] = L'x';
    annotation.boundary = (RECT){0, 0, 1, 1};
    CHECK(golden_document_serialize_utf8(&annotation, 1, NULL) == NULL);
}

static void test_document_control_characters(void) {
    Annotation source = {0}, parsed[2] = {0};
    source.name[0] = L'x';
    for (int i = 1; i < 32; ++i) source.name[i] = (wchar_t)i;
    source.name[32] = L'y';
    source.boundary = (RECT){-10, -20, 30, 40};
    size_t length = 0;
    char *json = golden_document_serialize_utf8(&source, 1, &length);
    int count = 2;
    CHECK(json != NULL);
    CHECK(json && golden_document_parse_utf8(json, length, parsed, &count));
    CHECK(count == 1 && !wcscmp(source.name, parsed[0].name));
    CHECK(count == 1 && EqualRect(&source.boundary, &parsed[0].boundary));
    free(json);
}

static void test_document_truncation_and_utf8(void) {
    Annotation source = {0}, parsed[2] = {0};
    wcscpy(source.name, L"truncate");
    source.boundary = (RECT){1, 2, 3, 4};
    size_t length = 0;
    char *json = golden_document_serialize_utf8(&source, 1, &length);
    CHECK(json != NULL);
    char *closing = json ? strrchr(json, '}') : NULL;
    if (closing) {
        for (size_t truncated = 0; truncated < (size_t)(closing - json + 1);
             ++truncated) {
            int count = 2;
            CHECK(!golden_document_parse_utf8(json, truncated, parsed, &count));
        }
    }
    free(json);

    const char *valid_utf8 =
        "{\"annotations\":[{\"name\":\"caf\xc3\xa9\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}}]}";
    int count = 2;
    CHECK(parse_document(valid_utf8, parsed, &count));
    CHECK(count == 1 && parsed[0].name[3] == 0x00e9);
    check_invalid_document(
        "{\"annotations\":[{\"name\":\"bad\xc0\xaf\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}}]}");
    check_invalid_document(
        "{\"annotations\":[{\"name\":\"bad\xed\xa0\x80\",\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}}]}");
}

static void test_document_capacity_boundary(void) {
    Annotation *source = (Annotation *)calloc(MAX_ANNOTATIONS, sizeof(*source));
    Annotation *parsed = (Annotation *)calloc(MAX_ANNOTATIONS, sizeof(*parsed));
    CHECK(source != NULL && parsed != NULL);
    if (!source || !parsed) { free(source); free(parsed); return; }
    for (int i = 0; i < MAX_ANNOTATIONS; ++i) {
        _snwprintf(source[i].name, _countof(source[i].name), L"item_%d", i);
        source[i].boundary = (RECT){i, i + 1, i + 2, i + 3};
    }
    size_t length = 0;
    char *json = golden_document_serialize_utf8(source, MAX_ANNOTATIONS, &length);
    CHECK(json != NULL);
    int count = MAX_ANNOTATIONS;
    CHECK(json && golden_document_parse_utf8(json, length, parsed, &count));
    CHECK(count == MAX_ANNOTATIONS);
    for (int i = 0; i < count; ++i) {
        CHECK(!wcscmp(source[i].name, parsed[i].name));
        CHECK(EqualRect(&source[i].boundary, &parsed[i].boundary));
    }
    count = MAX_ANNOTATIONS - 1;
    CHECK(json && !golden_document_parse_utf8(json, length, parsed, &count));
    free(json);
    free(source);
    free(parsed);
}

static void test_document_nesting_limit(void) {
    char json[512];
    const char *prefix = "{\"annotations\":[],\"future\":";
    size_t prefix_length = strlen(prefix);
    for (int nesting = 64; nesting <= 65; ++nesting) {
        memcpy(json, prefix, prefix_length);
        size_t cursor = prefix_length;
        for (int depth = 0; depth < nesting; ++depth) json[cursor++] = '[';
        memcpy(json + cursor, "null", 4);
        cursor += 4;
        for (int depth = 0; depth < nesting; ++depth) json[cursor++] = ']';
        json[cursor++] = '}';
        json[cursor] = 0;
        Annotation parsed[1] = {0};
        int count = 1;
        CHECK(parse_document(json, parsed, &count) == (nesting == 64));
        CHECK(nesting != 64 || count == 0);
    }
}

static void test_document_long_unknown_keys(void) {
    char key[513];
    memset(key, 'k', sizeof(key) - 1);
    key[sizeof(key) - 1] = 0;
    char json[4096];
    int length = snprintf(json, sizeof(json),
        "{\"%s\":true,\"\\u0000future\":false,\"annotations\":[{"
        "\"name\":\"long-keys\",\"%s\":null,"
        "\"boundary\":{\"x\":1,\"%s\":[],\"y\":2,"
        "\"width\":3,\"height\":4},"
        "\"click\":{\"x\":0.25,\"%s\":{},\"y\":0.75}}]}",
        key, key, key, key);
    CHECK(length > 0 && (size_t)length < sizeof(json));
    Annotation parsed[2] = {0};
    int count = 2;
    CHECK(length > 0 && (size_t)length < sizeof(json) &&
          golden_document_parse_utf8(json, (size_t)length, parsed, &count));
    CHECK(count == 1);
    CHECK(count == 1 && !wcscmp(parsed[0].name, L"long-keys"));
    CHECK(count == 1 && EqualRect(&parsed[0].boundary, &(RECT){1, 2, 4, 6}));
    CHECK(count == 1 && parsed[0].has_click &&
          parsed[0].click_x == 0.25 && parsed[0].click_y == 0.75);

    length = snprintf(json, sizeof(json),
        "{\"annotations%s\":null,\"annotations\":[]}", key);
    count = 2;
    CHECK(length > 0 && (size_t)length < sizeof(json) &&
          golden_document_parse_utf8(json, (size_t)length, parsed, &count));
    CHECK(count == 0);
}

int main(void) {
    test_annotation_names();
    test_rectangles_and_clicks();
    test_viewport_fidelity();
    test_document_round_trip();
    test_document_special_names();
    test_document_valid_variants();
    test_document_rejects_invalid_json();
    test_document_rejects_duplicates_and_truncation();
    test_document_rejects_invalid_model();
    test_document_control_characters();
    test_document_truncation_and_utf8();
    test_document_capacity_boundary();
    test_document_nesting_limit();
    test_document_long_unknown_keys();
    if (failures) {
        fprintf(stderr, "%d model test(s) failed.\n", failures);
        return 1;
    }
    puts("All Goldens model tests passed.");
    return 0;
}

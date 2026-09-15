#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "../src/namespace.h"
#include "../src/resource_ops.h"

static int write_bytes(const wchar_t *path, const char *data) {
    FILE *file = _wfopen(path, L"wb");
    if (!file) return 0;
    size_t length = strlen(data);
    int ok = fwrite(data, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}

static int write_resource(const wchar_t *directory, const wchar_t *stem,
                          const char *annotation_name,
                          wchar_t *png, size_t capacity) {
    wchar_t json[MAX_PATH];
    if (!golden_path_join_extension(directory, stem, L".png", png, capacity) ||
        !golden_resource_json_path(png, json, _countof(json)) ||
        !write_bytes(png, "png")) return 0;
    char document[512];
    int length = snprintf(document, sizeof(document),
        "{\"annotations\":[{\"name\":\"%s\","
        "\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}}]}",
        annotation_name);
    return length > 0 && (size_t)length < sizeof(document) &&
           write_bytes(json, document);
}

static void delete_resource(const wchar_t *png) {
    wchar_t json[MAX_PATH];
    if (golden_resource_json_path(png, json, _countof(json))) DeleteFileW(json);
    DeleteFileW(png);
}

int main(void) {
    wchar_t temporary[MAX_PATH], seed[MAX_PATH], root[MAX_PATH], child[MAX_PATH];
    if (!GetTempPathW(_countof(temporary), temporary) ||
        !GetTempFileNameW(temporary, L"gln", 0, seed)) return 1;
    DeleteFileW(seed);
    wcscpy(root, seed);
    if (!CreateDirectoryW(root, NULL) ||
        !golden_path_join(root, L"child", child, _countof(child)) ||
        !CreateDirectoryW(child, NULL)) return 1;

    wchar_t first[MAX_PATH], second[MAX_PATH], nested[MAX_PATH];
    wchar_t nested_collision[MAX_PATH] = L"";
    int failed = !write_resource(root, L"first", "submit", first,
                                 _countof(first)) ||
                 !write_resource(root, L"second", "cancel", second,
                                 _countof(second)) ||
                 !write_resource(child, L"screen", "submit", nested,
                                 _countof(nested));
    GoldenNamespaceIssue issue;
    if (!failed) failed = golden_namespace_validate_directory(
        root, NULL, 0, NULL, NULL, NULL, &issue) != GOLDEN_NAMESPACE_OK;
    if (!failed) failed = golden_namespace_validate_directory(
        child, NULL, 0, NULL, NULL, NULL, &issue) != GOLDEN_NAMESPACE_OK;
    if (!failed) failed = !write_resource(
        child, L"other-screen", "SUBMIT", nested_collision,
        _countof(nested_collision)) ||
        golden_namespace_validate_tree(root, &issue) !=
            GOLDEN_NAMESPACE_DUPLICATE;
    delete_resource(nested_collision);

    Annotation candidate = {0};
    wcscpy(candidate.name, L"SUBMIT");
    candidate.boundary = (RECT){0, 0, 1, 1};
    if (!failed) failed = golden_namespace_validate_directory(
        root, &candidate, 1, L"candidate.png", NULL, NULL, &issue) !=
            GOLDEN_NAMESPACE_DUPLICATE ||
        _wcsicmp(issue.annotation, L"submit") ||
        _wcsicmp(issue.first_png, L"candidate.png") ||
        _wcsicmp(issue.second_png, first);

    BOOL found = FALSE;
    if (!failed) failed = golden_namespace_find_name(
        root, first, L"submit", &found, &issue) != GOLDEN_NAMESPACE_OK || found;
    if (!failed) failed = golden_namespace_find_name(
        root, NULL, L"submit", &found, &issue) != GOLDEN_NAMESPACE_OK || !found;

    BOOL marked[2] = {TRUE, TRUE};
    Annotation active[2] = {0};
    wcscpy(active[0].name, L"submit");
    wcscpy(active[1].name, L"local-only");
    if (!failed) failed = golden_namespace_mark_collisions(
        root, first, active, 2, marked, &issue) != GOLDEN_NAMESPACE_OK ||
        marked[0] || marked[1];

    wchar_t second_json[MAX_PATH];
    if (!failed) failed = !golden_resource_json_path(
        second, second_json, _countof(second_json)) ||
        !write_bytes(second_json,
            "{\"annotations\":[{\"name\":\"Submit\","
            "\"boundary\":{\"x\":0,\"y\":0,\"width\":1,\"height\":1}}]}");
    if (!failed) failed = golden_namespace_validate_directory(
        root, NULL, 0, NULL, NULL, NULL, &issue) !=
            GOLDEN_NAMESPACE_DUPLICATE ||
        _wcsicmp(issue.annotation, L"submit");
    if (!failed) failed = golden_namespace_mark_collisions(
        root, first, active, 2, marked, &issue) != GOLDEN_NAMESPACE_OK ||
        !marked[0] || marked[1];

    wcscpy(active[1].name, L"SUBMIT");
    if (!failed) failed = golden_namespace_mark_collisions(
        child, nested, active, 2, marked, &issue) != GOLDEN_NAMESPACE_OK ||
        !marked[0] || !marked[1];

    if (!failed) failed = !write_bytes(second_json, "not-json") ||
        golden_namespace_validate_directory(
            root, NULL, 0, NULL, NULL, NULL, &issue) !=
            GOLDEN_NAMESPACE_INVALID_SIDECAR ||
        _wcsicmp(issue.first_png, second);

    delete_resource(first);
    delete_resource(second);
    delete_resource(nested);
    RemoveDirectoryW(child);
    RemoveDirectoryW(root);
    if (failed) return 1;
    puts("All Goldens namespace tests passed.");
    return 0;
}

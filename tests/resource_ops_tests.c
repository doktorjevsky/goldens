#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/document.h"
#include "../src/resource_ops.h"

static int write_bytes(const wchar_t *path, const void *data, size_t length) {
    FILE *file = _wfopen(path, L"wb");
    if (!file) return 0;
    int ok = fwrite(data, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}

static char *read_bytes(const wchar_t *path, size_t *length) {
    FILE *file = _wfopen(path, L"rb");
    if (!file || fseek(file, 0, SEEK_END)) { if (file) fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    char *data = (char *)malloc((size_t)size + 1);
    if (!data) { fclose(file); return NULL; }
    *length = fread(data, 1, (size_t)size, file);
    data[*length] = 0;
    fclose(file);
    return data;
}

int main(void) {
    wchar_t temporary[MAX_PATH], seed[MAX_PATH], directory[MAX_PATH];
    if (!GetTempPathW(_countof(temporary), temporary) ||
        !GetTempFileNameW(temporary, L"gld", 0, seed)) return 1;
    DeleteFileW(seed);
    wcscpy(directory, seed);
    if (!CreateDirectoryW(directory, NULL)) return 1;

    wchar_t old_png[MAX_PATH], new_png[MAX_PATH], old_json[MAX_PATH], new_json[MAX_PATH];
    _snwprintf(old_png, _countof(old_png), L"%s\\before.png", directory);
    _snwprintf(new_png, _countof(new_png), L"%s\\after.png", directory);
    golden_resource_json_path(old_png, old_json, _countof(old_json));
    golden_resource_json_path(new_png, new_json, _countof(new_json));
    BYTE png[] = {1, 2, 3, 4};
    Annotation annotation = {0};
    wcscpy(annotation.name, L"before_save");
    annotation.boundary = (RECT){1, 2, 11, 22};
    size_t json_length = 0;
    char *json = golden_document_serialize_utf8(&annotation, 1, &json_length);
    int failed = !json || !write_bytes(old_png, png, sizeof(png)) ||
                 !write_bytes(old_json, json, json_length);
    free(json);

    if (!failed) failed = golden_rename_resource_pair(old_png, new_png) != GOLDEN_RENAME_OK;
    if (!failed) failed = GetFileAttributesW(old_png) != INVALID_FILE_ATTRIBUTES ||
                          GetFileAttributesW(old_json) != INVALID_FILE_ATTRIBUTES ||
                          GetFileAttributesW(new_png) == INVALID_FILE_ATTRIBUTES ||
                          GetFileAttributesW(new_json) == INVALID_FILE_ATTRIBUTES;

    wcscpy(annotation.name, L"after_save");
    json = golden_document_serialize_utf8(&annotation, 1, &json_length);
    if (!failed) failed = !json || !write_bytes(new_json, json, json_length);
    free(json);
    size_t saved_length = 0;
    char *saved = failed ? NULL : read_bytes(new_json, &saved_length);
    Annotation parsed[2] = {0};
    int count = 2;
    if (!failed) failed = !saved || !golden_document_parse_utf8(saved, saved_length, parsed, &count) ||
                          count != 1 || wcscmp(parsed[0].name, L"after_save");
    free(saved);

    wchar_t collision_png[MAX_PATH];
    _snwprintf(collision_png, _countof(collision_png), L"%s\\collision.png", directory);
    if (!failed) failed = !write_bytes(collision_png, png, sizeof(png)) ||
        golden_rename_resource_pair(new_png, collision_png) != GOLDEN_RENAME_PNG_EXISTS;

    DeleteFileW(old_png); DeleteFileW(old_json);
    DeleteFileW(new_png); DeleteFileW(new_json); DeleteFileW(collision_png);
    RemoveDirectoryW(directory);
    if (failed) return 1;
    puts("All Goldens resource rename tests passed.");
    return 0;
}

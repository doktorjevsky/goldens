#include "resource_ops.h"

#include <wchar.h>

void golden_resource_json_path(const wchar_t *png_path, wchar_t *output, size_t capacity) {
    if (!output || !capacity) return;
    wcsncpy(output, png_path ? png_path : L"", capacity - 1);
    output[capacity - 1] = 0;
    wchar_t *slash = wcsrchr(output, L'\\');
    wchar_t *dot = wcsrchr(output, L'.');
    if (dot && (!slash || dot > slash)) {
        size_t remaining = capacity - (size_t)(dot - output);
        if (remaining >= 6) wcscpy(dot, L".json");
    }
}

GoldenResourceRenameResult golden_rename_resource_pair(const wchar_t *old_png,
                                                       const wchar_t *new_png) {
    if (!old_png || !new_png || GetFileAttributesW(old_png) == INVALID_FILE_ATTRIBUTES)
        return GOLDEN_RENAME_SOURCE_MISSING;
    if (!wcscmp(old_png, new_png)) return GOLDEN_RENAME_OK;
    BOOL same_png = !_wcsicmp(old_png, new_png);
    if (!same_png && GetFileAttributesW(new_png) != INVALID_FILE_ATTRIBUTES)
        return GOLDEN_RENAME_PNG_EXISTS;

    wchar_t old_json[MAX_PATH * 4], new_json[MAX_PATH * 4];
    golden_resource_json_path(old_png, old_json, _countof(old_json));
    golden_resource_json_path(new_png, new_json, _countof(new_json));
    BOOL has_json = GetFileAttributesW(old_json) != INVALID_FILE_ATTRIBUTES;
    BOOL same_json = !_wcsicmp(old_json, new_json);
    if (has_json && !same_json && GetFileAttributesW(new_json) != INVALID_FILE_ATTRIBUTES)
        return GOLDEN_RENAME_JSON_EXISTS;

    DWORD flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH;
    if (!MoveFileExW(old_png, new_png, flags)) return GOLDEN_RENAME_PNG_FAILED;
    if (has_json && !MoveFileExW(old_json, new_json, flags)) {
        MoveFileExW(new_png, old_png, flags);
        return GOLDEN_RENAME_JSON_FAILED_ROLLED_BACK;
    }
    return GOLDEN_RENAME_OK;
}

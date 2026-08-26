#include "resource_ops.h"

#include <stdint.h>
#include <string.h>
#include <wchar.h>

static BOOL add_size(size_t *total, size_t amount) {
    if (amount > SIZE_MAX - *total) return FALSE;
    *total += amount;
    return TRUE;
}

BOOL golden_path_copy(const wchar_t *path, wchar_t *output, size_t capacity) {
    if (!output || !capacity) return FALSE;
    if (!path) { output[0] = 0; return FALSE; }
    size_t length = wcslen(path);
    if (length >= capacity) { output[0] = 0; return FALSE; }
    memmove(output, path, (length + 1) * sizeof(*output));
    return TRUE;
}

static BOOL join_path_parts(const wchar_t *directory, const wchar_t *leaf,
                            const wchar_t *suffix,
                            wchar_t *output, size_t capacity) {
    if (!output || !capacity) return FALSE;
    if (!directory || !leaf || !suffix) { output[0] = 0; return FALSE; }

    size_t directory_length = wcslen(directory);
    size_t leaf_length = wcslen(leaf);
    size_t suffix_length = wcslen(suffix);
    BOOL needs_separator = directory_length &&
        directory[directory_length - 1] != L'\\' &&
        directory[directory_length - 1] != L'/';
    size_t required = directory_length;
    if (!add_size(&required, needs_separator ? 1u : 0u) ||
        !add_size(&required, leaf_length) ||
        !add_size(&required, suffix_length) ||
        !add_size(&required, 1u) || required > capacity) {
        output[0] = 0;
        return FALSE;
    }

    wchar_t *cursor = output;
    memmove(cursor, directory, directory_length * sizeof(*cursor));
    cursor += directory_length;
    if (needs_separator) *cursor++ = L'\\';
    memmove(cursor, leaf, leaf_length * sizeof(*cursor));
    cursor += leaf_length;
    memmove(cursor, suffix, (suffix_length + 1) * sizeof(*cursor));
    return TRUE;
}

BOOL golden_path_join(const wchar_t *directory, const wchar_t *leaf,
                      wchar_t *output, size_t capacity) {
    return join_path_parts(directory, leaf, L"", output, capacity);
}

BOOL golden_path_join_extension(const wchar_t *directory, const wchar_t *stem,
                                const wchar_t *extension,
                                wchar_t *output, size_t capacity) {
    if (!extension || extension[0] != L'.' || !extension[1]) {
        if (output && capacity) output[0] = 0;
        return FALSE;
    }
    return join_path_parts(directory, stem, extension, output, capacity);
}

BOOL golden_resource_json_path(const wchar_t *png_path,
                               wchar_t *output, size_t capacity) {
    if (!output || !capacity) return FALSE;
    if (!png_path) { output[0] = 0; return FALSE; }
    size_t length = wcslen(png_path);
    if (length < 4 || _wcsicmp(png_path + length - 4, L".png")) {
        output[0] = 0;
        return FALSE;
    }
    size_t stem_length = length - 4;
    if (stem_length > SIZE_MAX - 6 || stem_length + 6 > capacity) {
        output[0] = 0;
        return FALSE;
    }
    memmove(output, png_path, stem_length * sizeof(*output));
    memcpy(output + stem_length, L".json", 6 * sizeof(*output));
    return TRUE;
}

static BOOL move_file(const wchar_t *source, const wchar_t *destination,
                      DWORD flags, void *context) {
    UNREFERENCED_PARAMETER(context);
    return MoveFileExW(source, destination, flags);
}

GoldenResourceRenameResult golden_rename_resource_pair_with_move(
    const wchar_t *old_png, const wchar_t *new_png,
    GoldenMoveFileOperation operation, void *context) {
    if (!old_png || !new_png || !operation ||
        GetFileAttributesW(old_png) == INVALID_FILE_ATTRIBUTES)
        return GOLDEN_RENAME_SOURCE_MISSING;
    if (!wcscmp(old_png, new_png)) return GOLDEN_RENAME_OK;
    BOOL same_png = !_wcsicmp(old_png, new_png);
    if (!same_png && GetFileAttributesW(new_png) != INVALID_FILE_ATTRIBUTES)
        return GOLDEN_RENAME_PNG_EXISTS;

    wchar_t old_json[MAX_PATH * 4], new_json[MAX_PATH * 4];
    if (!golden_resource_json_path(old_png, old_json, _countof(old_json)) ||
        !golden_resource_json_path(new_png, new_json, _countof(new_json)))
        return GOLDEN_RENAME_INVALID_PATH;
    BOOL has_json = GetFileAttributesW(old_json) != INVALID_FILE_ATTRIBUTES;
    BOOL same_json = !_wcsicmp(old_json, new_json);
    if (has_json && !same_json && GetFileAttributesW(new_json) != INVALID_FILE_ATTRIBUTES)
        return GOLDEN_RENAME_JSON_EXISTS;

    DWORD flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH;
    if (!operation(old_png, new_png, flags, context)) return GOLDEN_RENAME_PNG_FAILED;
    if (has_json && !operation(old_json, new_json, flags, context)) {
        return operation(new_png, old_png, flags, context) ?
            GOLDEN_RENAME_JSON_FAILED_ROLLED_BACK : GOLDEN_RENAME_ROLLBACK_FAILED;
    }
    return GOLDEN_RENAME_OK;
}

GoldenResourceRenameResult golden_rename_resource_pair(const wchar_t *old_png,
                                                       const wchar_t *new_png) {
    return golden_rename_resource_pair_with_move(old_png, new_png,
                                                 move_file, NULL);
}

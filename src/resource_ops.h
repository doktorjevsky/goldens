#ifndef GOLDENS_RESOURCE_OPS_H
#define GOLDENS_RESOURCE_OPS_H

#include <windows.h>
#include <stddef.h>

typedef enum {
    GOLDEN_RENAME_OK,
    GOLDEN_RENAME_SOURCE_MISSING,
    GOLDEN_RENAME_INVALID_PATH,
    GOLDEN_RENAME_PNG_EXISTS,
    GOLDEN_RENAME_JSON_EXISTS,
    GOLDEN_RENAME_PNG_FAILED,
    GOLDEN_RENAME_JSON_FAILED_ROLLED_BACK,
    GOLDEN_RENAME_ROLLBACK_FAILED
} GoldenResourceRenameResult;

typedef BOOL (*GoldenMoveFileOperation)(const wchar_t *source,
                                        const wchar_t *destination,
                                        DWORD flags, void *context);

BOOL golden_path_copy(const wchar_t *path, wchar_t *output, size_t capacity);
BOOL golden_path_join(const wchar_t *directory, const wchar_t *leaf,
                      wchar_t *output, size_t capacity);
BOOL golden_path_join_extension(const wchar_t *directory, const wchar_t *stem,
                                const wchar_t *extension,
                                wchar_t *output, size_t capacity);
BOOL golden_resource_json_path(const wchar_t *png_path,
                               wchar_t *output, size_t capacity);
GoldenResourceRenameResult golden_rename_resource_pair(const wchar_t *old_png,
                                                       const wchar_t *new_png);
GoldenResourceRenameResult golden_rename_resource_pair_with_move(
    const wchar_t *old_png, const wchar_t *new_png,
    GoldenMoveFileOperation move_file, void *context);

#endif

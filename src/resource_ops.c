#include "resource_ops.h"
#include "atomic_file.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define RESOURCE_JOURNAL_MAGIC UINT32_C(0x474d4f56)

typedef struct {
    uint32_t magic;
    uint32_t version;
    BOOL has_json;
    wchar_t old_png[MAX_PATH * 4];
    wchar_t new_png[MAX_PATH * 4];
} ResourceMoveJournal;

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

static BOOL move_file(const wchar_t *source, const wchar_t *destination,
                      DWORD flags, void *context) {
    UNREFERENCED_PARAMETER(context);
    return MoveFileExW(source, destination, flags);
}

BOOL golden_path_is_same_or_inside(const wchar_t *path,
                                   const wchar_t *directory) {
    if (!path || !directory) return FALSE;
    size_t length = wcslen(directory);
    while (length && (directory[length - 1] == L'\\' ||
                      directory[length - 1] == L'/')) --length;
    return !_wcsnicmp(path, directory, length) &&
           (!path[length] || path[length] == L'\\' || path[length] == L'/');
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
    golden_resource_json_path(old_png, old_json, _countof(old_json));
    golden_resource_json_path(new_png, new_json, _countof(new_json));
    BOOL has_json = GetFileAttributesW(old_json) != INVALID_FILE_ATTRIBUTES;
    BOOL same_json = !_wcsicmp(old_json, new_json);
    if (!same_json && GetFileAttributesW(new_json) != INVALID_FILE_ATTRIBUTES)
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

static BOOL path_exists(const wchar_t *path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

GoldenResourceRenameResult golden_rename_resource_pair_transactional_with_move(
    const wchar_t *old_png, const wchar_t *new_png,
    const wchar_t *journal_path,
    GoldenMoveFileOperation operation, void *context) {
    if (!journal_path || !journal_path[0]) return GOLDEN_RENAME_JOURNAL_FAILED;
    if (path_exists(journal_path)) {
        GoldenResourceRenameResult recovered =
            golden_recover_resource_pair_move(journal_path);
        if (recovered != GOLDEN_RENAME_OK) return recovered;
    }
    ResourceMoveJournal journal = {0};
    journal.magic = RESOURCE_JOURNAL_MAGIC;
    journal.version = 1;
    wcsncpy(journal.old_png, old_png ? old_png : L"",
            _countof(journal.old_png) - 1);
    wcsncpy(journal.new_png, new_png ? new_png : L"",
            _countof(journal.new_png) - 1);
    wchar_t old_json[MAX_PATH * 4];
    golden_resource_json_path(old_png, old_json, _countof(old_json));
    journal.has_json = path_exists(old_json);
    if (!golden_atomic_write_bytes(journal_path, &journal, sizeof(journal)))
        return GOLDEN_RENAME_JOURNAL_FAILED;

    GoldenResourceRenameResult result = golden_rename_resource_pair_with_move(
        old_png, new_png, operation, context);
    if (result != GOLDEN_RENAME_ROLLBACK_FAILED) DeleteFileW(journal_path);
    return result;
}

GoldenResourceRenameResult golden_rename_resource_pair_transactional(
    const wchar_t *old_png, const wchar_t *new_png,
    const wchar_t *journal_path) {
    return golden_rename_resource_pair_transactional_with_move(
        old_png, new_png, journal_path, move_file, NULL);
}

GoldenResourceRenameResult golden_recover_resource_pair_move(
    const wchar_t *journal_path) {
    if (!journal_path || !journal_path[0] || !path_exists(journal_path))
        return GOLDEN_RENAME_OK;
    ResourceMoveJournal journal = {0};
    FILE *file = _wfopen(journal_path, L"rb");
    if (!file) return GOLDEN_RENAME_RECOVERY_FAILED;
    size_t read = fread(&journal, 1, sizeof(journal), file);
    BOOL extra = fgetc(file) != EOF;
    fclose(file);
    if (read != sizeof(journal) || extra ||
        journal.magic != RESOURCE_JOURNAL_MAGIC || journal.version != 1 ||
        !journal.old_png[0] || !journal.new_png[0] ||
        journal.old_png[_countof(journal.old_png) - 1] ||
        journal.new_png[_countof(journal.new_png) - 1])
        return GOLDEN_RENAME_RECOVERY_FAILED;

    wchar_t old_json_path[MAX_PATH * 4], new_json_path[MAX_PATH * 4];
    golden_resource_json_path(journal.old_png, old_json_path,
                              _countof(old_json_path));
    golden_resource_json_path(journal.new_png, new_json_path,
                              _countof(new_json_path));
    BOOL old_png = path_exists(journal.old_png);
    BOOL new_png = path_exists(journal.new_png);
    BOOL old_json = path_exists(old_json_path);
    BOOL new_json = path_exists(new_json_path);

    BOOL consistent_old = old_png && !new_png &&
        (!journal.has_json || (old_json && !new_json));
    BOOL consistent_new = new_png && !old_png &&
        (!journal.has_json || (new_json && !old_json));
    if (!consistent_old && !consistent_new && new_png && !old_png &&
        journal.has_json && old_json && !new_json) {
        DWORD flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH;
        if (MoveFileExW(old_json_path, new_json_path, flags))
            consistent_new = TRUE;
        else if (MoveFileExW(journal.new_png, journal.old_png, flags))
            consistent_old = TRUE;
    }
    if (!consistent_old && !consistent_new)
        return GOLDEN_RENAME_RECOVERY_FAILED;
    return DeleteFileW(journal_path) || GetLastError() == ERROR_FILE_NOT_FOUND ?
        GOLDEN_RENAME_OK : GOLDEN_RENAME_RECOVERY_FAILED;
}

GoldenDirectoryMoveResult golden_move_directory_with_move(
    const wchar_t *source, const wchar_t *destination,
    GoldenMoveFileOperation operation, void *context) {
    if (!source || !destination || !operation)
        return GOLDEN_DIRECTORY_MOVE_SOURCE_MISSING;
    DWORD source_attributes = GetFileAttributesW(source);
    if (source_attributes == INVALID_FILE_ATTRIBUTES)
        return GOLDEN_DIRECTORY_MOVE_SOURCE_MISSING;
    if (!(source_attributes & FILE_ATTRIBUTE_DIRECTORY))
        return GOLDEN_DIRECTORY_MOVE_SOURCE_NOT_DIRECTORY;
    if (!wcscmp(source, destination)) return GOLDEN_DIRECTORY_MOVE_OK;
    BOOL same_path_ignoring_case = !_wcsicmp(source, destination);
    if (!same_path_ignoring_case &&
        GetFileAttributesW(destination) != INVALID_FILE_ATTRIBUTES)
        return GOLDEN_DIRECTORY_MOVE_DESTINATION_EXISTS;
    if (!same_path_ignoring_case &&
        golden_path_is_same_or_inside(destination, source))
        return GOLDEN_DIRECTORY_MOVE_DESTINATION_INSIDE_SOURCE;
    return operation(source, destination, MOVEFILE_WRITE_THROUGH, context) ?
        GOLDEN_DIRECTORY_MOVE_OK : GOLDEN_DIRECTORY_MOVE_FAILED;
}

GoldenDirectoryMoveResult golden_move_directory(const wchar_t *source,
                                                const wchar_t *destination) {
    return golden_move_directory_with_move(source, destination,
                                           move_file, NULL);
}

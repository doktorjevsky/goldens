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

static int file_equals(const wchar_t *path, const char *expected,
                       size_t expected_length) {
    size_t length = 0;
    char *data = read_bytes(path, &length);
    int equal = data && length == expected_length &&
                !memcmp(data, expected, expected_length);
    free(data);
    return equal;
}

typedef struct {
    int calls;
    unsigned failures;
} MoveContext;

static BOOL controlled_move(const wchar_t *source, const wchar_t *destination,
                            DWORD flags, void *opaque) {
    MoveContext *context = (MoveContext *)opaque;
    int call = ++context->calls;
    if (context->failures & (1u << call)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return MoveFileExW(source, destination, flags);
}

static int test_checked_paths(void) {
    wchar_t path[64];
    wchar_t too_small[9] = L"sentinel";
    int failed =
        !golden_path_copy(L"C:\\root", path, _countof(path)) ||
        wcscmp(path, L"C:\\root") ||
        golden_path_copy(L"C:\\root", too_small, 7) ||
        too_small[0] != 0 ||
        !golden_path_join(L"C:\\root", L"image.png", path, _countof(path)) ||
        wcscmp(path, L"C:\\root\\image.png") ||
        !golden_path_join(L"C:\\root\\", L"image.png", path, _countof(path)) ||
        wcscmp(path, L"C:\\root\\image.png") ||
        !golden_path_join_extension(L"C:\\root", L"image", L".png",
                                    path, _countof(path)) ||
        wcscmp(path, L"C:\\root\\image.png") ||
        golden_path_join_extension(L"C:\\root", L"image", L"png",
                                   path, _countof(path)) ||
        path[0] != 0 ||
        !golden_resource_json_path(L"C:\\root\\image.PNG",
                                   path, _countof(path)) ||
        wcscmp(path, L"C:\\root\\image.json") ||
        !golden_path_is_same_or_inside(L"C:\\root", L"C:\\root") ||
        !golden_path_is_same_or_inside(L"C:\\root\\nested\\image.png",
                                       L"C:\\root") ||
        !golden_path_is_same_or_inside(L"c:\\ROOT\\nested",
                                       L"C:\\root\\") ||
        golden_path_is_same_or_inside(L"C:\\rooted", L"C:\\root") ||
        golden_path_is_same_or_inside(L"C:\\other", L"C:\\root") ||
        golden_resource_json_path(L"C:\\root\\image", path, _countof(path)) ||
        path[0] != 0 ||
        golden_resource_json_path(L"foo.png", too_small, 8) ||
        too_small[0] != 0;
    return failed;
}

static int make_pair(const wchar_t *directory, const wchar_t *stem,
                     wchar_t *old_png, wchar_t *new_png,
                     wchar_t *old_json, wchar_t *new_json) {
    _snwprintf(old_png, MAX_PATH, L"%s\\%s-before.png", directory, stem);
    _snwprintf(new_png, MAX_PATH, L"%s\\%s-after.png", directory, stem);
    return golden_resource_json_path(old_png, old_json, MAX_PATH) &&
        golden_resource_json_path(new_png, new_json, MAX_PATH) &&
        write_bytes(old_png, "png", 3) && write_bytes(old_json, "json", 4);
}

static int test_json_failure_rolls_back(const wchar_t *directory) {
    wchar_t old_png[MAX_PATH], new_png[MAX_PATH], old_json[MAX_PATH], new_json[MAX_PATH];
    if (!make_pair(directory, L"rollback", old_png, new_png, old_json, new_json)) return 1;
    MoveContext context = {0, 1u << 2};
    GoldenResourceRenameResult result = golden_rename_resource_pair_with_move(
        old_png, new_png, controlled_move, &context);
    int failed = result != GOLDEN_RENAME_JSON_FAILED_ROLLED_BACK ||
        context.calls != 3 ||
        GetFileAttributesW(old_png) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(old_json) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(new_png) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(new_json) != INVALID_FILE_ATTRIBUTES;
    DeleteFileW(old_png); DeleteFileW(old_json);
    DeleteFileW(new_png); DeleteFileW(new_json);
    return failed;
}

static int test_rollback_failure_is_reported(const wchar_t *directory) {
    wchar_t old_png[MAX_PATH], new_png[MAX_PATH], old_json[MAX_PATH], new_json[MAX_PATH];
    if (!make_pair(directory, L"rollback-fails", old_png, new_png, old_json, new_json)) return 1;
    MoveContext context = {0, (1u << 2) | (1u << 3)};
    GoldenResourceRenameResult result = golden_rename_resource_pair_with_move(
        old_png, new_png, controlled_move, &context);
    int failed = result != GOLDEN_RENAME_ROLLBACK_FAILED || context.calls != 3 ||
        GetFileAttributesW(old_png) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(old_json) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(new_png) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(new_json) != INVALID_FILE_ATTRIBUTES;
    DeleteFileW(old_png); DeleteFileW(old_json);
    DeleteFileW(new_png); DeleteFileW(new_json);
    return failed;
}

static int test_transaction_journal_recovers_split_pair(const wchar_t *directory) {
    wchar_t old_png[MAX_PATH], new_png[MAX_PATH], old_json[MAX_PATH], new_json[MAX_PATH];
    wchar_t journal[MAX_PATH];
    if (!make_pair(directory, L"journal-recovery", old_png, new_png,
                   old_json, new_json)) return 1;
    _snwprintf(journal, _countof(journal), L"%s\\move.journal", directory);
    MoveContext context = {0, (1u << 2) | (1u << 3)};
    GoldenResourceRenameResult result =
        golden_rename_resource_pair_transactional_with_move(
            old_png, new_png, journal, controlled_move, &context);
    int failed = result != GOLDEN_RENAME_ROLLBACK_FAILED ||
        GetFileAttributesW(journal) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(new_png) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(old_json) == INVALID_FILE_ATTRIBUTES;
    if (!failed)
        failed = golden_recover_resource_pair_move(journal) != GOLDEN_RENAME_OK ||
            GetFileAttributesW(journal) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(old_png) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(old_json) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(new_png) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(new_json) == INVALID_FILE_ATTRIBUTES;
    DeleteFileW(old_png); DeleteFileW(old_json);
    DeleteFileW(new_png); DeleteFileW(new_json); DeleteFileW(journal);
    return failed;
}

static int test_first_move_failure_is_reported(const wchar_t *directory) {
    wchar_t old_png[MAX_PATH], new_png[MAX_PATH], old_json[MAX_PATH], new_json[MAX_PATH];
    if (!make_pair(directory, L"first-fails", old_png, new_png, old_json, new_json)) return 1;
    MoveContext context = {0, 1u << 1};
    GoldenResourceRenameResult result = golden_rename_resource_pair_with_move(
        old_png, new_png, controlled_move, &context);
    int failed = result != GOLDEN_RENAME_PNG_FAILED || context.calls != 1 ||
        GetFileAttributesW(old_png) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(old_json) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(new_png) != INVALID_FILE_ATTRIBUTES;
    DeleteFileW(old_png); DeleteFileW(old_json);
    DeleteFileW(new_png); DeleteFileW(new_json);
    return failed;
}

static int test_pair_moves_between_directories(const wchar_t *directory) {
    wchar_t target_directory[MAX_PATH];
    wchar_t source_png[MAX_PATH], source_json[MAX_PATH];
    wchar_t target_png[MAX_PATH], target_json[MAX_PATH];
    _snwprintf(target_directory, _countof(target_directory),
               L"%s\\move-target", directory);
    _snwprintf(source_png, _countof(source_png), L"%s\\move.png", directory);
    _snwprintf(target_png, _countof(target_png), L"%s\\move.png",
               target_directory);
    if (!CreateDirectoryW(target_directory, NULL) ||
        !golden_resource_json_path(source_png, source_json,
                                   _countof(source_json)) ||
        !golden_resource_json_path(target_png, target_json,
                                   _countof(target_json)) ||
        !write_bytes(source_png, "png", 3) ||
        !write_bytes(source_json, "json", 4))
        return 1;
    GoldenResourceRenameResult result =
        golden_rename_resource_pair(source_png, target_png);
    int failed = result != GOLDEN_RENAME_OK ||
        GetFileAttributesW(source_png) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(source_json) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(target_png) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(target_json) == INVALID_FILE_ATTRIBUTES;
    DeleteFileW(source_png); DeleteFileW(source_json);
    DeleteFileW(target_png); DeleteFileW(target_json);
    RemoveDirectoryW(target_directory);
    return failed;
}

static int test_orphan_destination_json_blocks_png_move(const wchar_t *directory) {
    wchar_t old_png[MAX_PATH], new_png[MAX_PATH], old_json[MAX_PATH], new_json[MAX_PATH];
    _snwprintf(old_png, _countof(old_png), L"%s\\orphan-before.png", directory);
    _snwprintf(new_png, _countof(new_png), L"%s\\orphan-after.png", directory);
    golden_resource_json_path(old_png, old_json, _countof(old_json));
    golden_resource_json_path(new_png, new_json, _countof(new_json));
    if (!write_bytes(old_png, "png", 3) || !write_bytes(new_json, "unrelated", 9)) return 1;
    GoldenResourceRenameResult result = golden_rename_resource_pair(old_png, new_png);
    int failed = result != GOLDEN_RENAME_JSON_EXISTS ||
        GetFileAttributesW(old_png) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(new_png) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(new_json) == INVALID_FILE_ATTRIBUTES;
    DeleteFileW(old_png); DeleteFileW(old_json);
    DeleteFileW(new_png); DeleteFileW(new_json);
    return failed;
}

static int test_resource_pair_move_undo_redo(const wchar_t *directory) {
    wchar_t first_png[MAX_PATH], second_png[MAX_PATH];
    wchar_t first_json[MAX_PATH], second_json[MAX_PATH];
    if (!make_pair(directory, L"round-trip", first_png, second_png,
                   first_json, second_json)) return 1;
    int failed = golden_rename_resource_pair(first_png, second_png) != GOLDEN_RENAME_OK ||
        golden_rename_resource_pair(second_png, first_png) != GOLDEN_RENAME_OK ||
        GetFileAttributesW(first_png) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(first_json) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(second_png) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(second_json) != INVALID_FILE_ATTRIBUTES;
    if (!failed)
        failed = golden_rename_resource_pair(first_png, second_png) != GOLDEN_RENAME_OK ||
            GetFileAttributesW(second_png) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(second_json) == INVALID_FILE_ATTRIBUTES;
    DeleteFileW(first_png); DeleteFileW(first_json);
    DeleteFileW(second_png); DeleteFileW(second_json);
    return failed;
}

static int test_resource_pair_copy(const wchar_t *directory) {
    wchar_t source_png[MAX_PATH], destination_png[MAX_PATH];
    wchar_t source_json[MAX_PATH], destination_json[MAX_PATH];
    if (!make_pair(directory, L"copy", source_png, destination_png,
                   source_json, destination_json)) return 1;
    int failed = !golden_copy_resource_pair(source_png, destination_png) ||
        GetFileAttributesW(source_png) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(source_json) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(destination_png) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(destination_json) == INVALID_FILE_ATTRIBUTES;
    size_t png_length = 0, json_length = 0;
    char *png = failed ? NULL : read_bytes(destination_png, &png_length);
    char *json = failed ? NULL : read_bytes(destination_json, &json_length);
    if (!failed) failed = !png || png_length != 3 || memcmp(png, "png", 3) ||
                          !json || json_length != 4 || memcmp(json, "json", 4) ||
                          golden_copy_resource_pair(source_png, destination_png);
    free(png); free(json);
    DeleteFileW(source_png); DeleteFileW(source_json);
    DeleteFileW(destination_png); DeleteFileW(destination_json);
    return failed;
}

static int test_resource_pair_copy_rolls_back(const wchar_t *directory) {
    wchar_t source_png[MAX_PATH], destination_png[MAX_PATH];
    wchar_t source_json[MAX_PATH], destination_json[MAX_PATH];
    _snwprintf(source_png, _countof(source_png),
               L"%s\\copy-rollback-before.png", directory);
    _snwprintf(destination_png, _countof(destination_png),
               L"%s\\copy-rollback-after.png", directory);
    if (!golden_resource_json_path(source_png, source_json,
                                   _countof(source_json)) ||
        !golden_resource_json_path(destination_png, destination_json,
                                   _countof(destination_json)) ||
        !write_bytes(source_png, "png", 3) ||
        !CreateDirectoryW(source_json, NULL)) return 1;
    int failed = golden_copy_resource_pair(source_png, destination_png) ||
        GetFileAttributesW(source_png) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(destination_png) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(destination_json) != INVALID_FILE_ATTRIBUTES;
    DeleteFileW(source_png); DeleteFileW(destination_png);
    DeleteFileW(destination_json); RemoveDirectoryW(source_json);
    return failed;
}

static int test_resource_pair_replace_undo_redo(const wchar_t *directory) {
    wchar_t source_png[MAX_PATH], destination_png[MAX_PATH], backup_png[MAX_PATH];
    wchar_t source_json[MAX_PATH], destination_json[MAX_PATH], backup_json[MAX_PATH];
    _snwprintf(source_png, _countof(source_png), L"%s\\replace-source.png", directory);
    _snwprintf(destination_png, _countof(destination_png),
               L"%s\\replace-destination.png", directory);
    _snwprintf(backup_png, _countof(backup_png), L"%s\\replace-backup.png", directory);
    if (!golden_resource_json_path(source_png, source_json, _countof(source_json)) ||
        !golden_resource_json_path(destination_png, destination_json,
                                   _countof(destination_json)) ||
        !golden_resource_json_path(backup_png, backup_json, _countof(backup_json)) ||
        !write_bytes(source_png, "source-png", 10) ||
        !write_bytes(source_json, "source-json", 11) ||
        !write_bytes(destination_png, "destination-png", 15) ||
        !write_bytes(destination_json, "destination-json", 16)) return 1;

    int failed = golden_rename_resource_pair(destination_png, backup_png) !=
                     GOLDEN_RENAME_OK ||
        golden_rename_resource_pair(source_png, destination_png) != GOLDEN_RENAME_OK ||
        !file_equals(destination_png, "source-png", 10) ||
        !file_equals(destination_json, "source-json", 11) ||
        !file_equals(backup_png, "destination-png", 15) ||
        !file_equals(backup_json, "destination-json", 16);
    if (!failed) failed =
        golden_rename_resource_pair(destination_png, source_png) != GOLDEN_RENAME_OK ||
        golden_rename_resource_pair(backup_png, destination_png) != GOLDEN_RENAME_OK ||
        !file_equals(source_png, "source-png", 10) ||
        !file_equals(source_json, "source-json", 11) ||
        !file_equals(destination_png, "destination-png", 15) ||
        !file_equals(destination_json, "destination-json", 16);
    if (!failed) failed =
        golden_rename_resource_pair(destination_png, backup_png) != GOLDEN_RENAME_OK ||
        golden_rename_resource_pair(source_png, destination_png) != GOLDEN_RENAME_OK ||
        !file_equals(destination_png, "source-png", 10) ||
        !file_equals(destination_json, "source-json", 11) ||
        !file_equals(backup_png, "destination-png", 15) ||
        !file_equals(backup_json, "destination-json", 16);
    DeleteFileW(source_png); DeleteFileW(source_json);
    DeleteFileW(destination_png); DeleteFileW(destination_json);
    DeleteFileW(backup_png); DeleteFileW(backup_json);
    return failed;
}

static int test_directory_move(const wchar_t *directory) {
    wchar_t source[MAX_PATH], destination[MAX_PATH], child[MAX_PATH];
    _snwprintf(source, _countof(source), L"%s\\folder-before", directory);
    _snwprintf(destination, _countof(destination), L"%s\\folder-after", directory);
    _snwprintf(child, _countof(child), L"%s\\inside.png", source);
    if (!CreateDirectoryW(source, NULL) || !write_bytes(child, "png", 3)) return 1;
    GoldenDirectoryMoveResult result = golden_move_directory(source, destination);
    _snwprintf(child, _countof(child), L"%s\\inside.png", destination);
    int failed = result != GOLDEN_DIRECTORY_MOVE_OK ||
        GetFileAttributesW(source) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(child) == INVALID_FILE_ATTRIBUTES;
    if (!failed) {
        result = golden_move_directory(destination, source);
        _snwprintf(child, _countof(child), L"%s\\inside.png", source);
        failed = result != GOLDEN_DIRECTORY_MOVE_OK ||
            GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(child) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(destination) != INVALID_FILE_ATTRIBUTES;
    }
    if (!failed) {
        result = golden_move_directory(source, destination);
        _snwprintf(child, _countof(child), L"%s\\inside.png", destination);
        failed = result != GOLDEN_DIRECTORY_MOVE_OK ||
            GetFileAttributesW(destination) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(child) == INVALID_FILE_ATTRIBUTES;
    }
    DeleteFileW(child);
    RemoveDirectoryW(source);
    RemoveDirectoryW(destination);
    return failed;
}

static int test_directory_create_undo_redo(const wchar_t *directory) {
    wchar_t path[MAX_PATH];
    _snwprintf(path, _countof(path), L"%s\\created-folder", directory);
    int failed = !CreateDirectoryW(path, NULL) || !RemoveDirectoryW(path) ||
        GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES ||
        !CreateDirectoryW(path, NULL) ||
        GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES;
    RemoveDirectoryW(path);
    return failed;
}

static int test_directory_move_rejects_descendant(const wchar_t *directory) {
    wchar_t source[MAX_PATH], destination[MAX_PATH];
    _snwprintf(source, _countof(source), L"%s\\parent", directory);
    _snwprintf(destination, _countof(destination), L"%s\\parent\\child\\parent", directory);
    if (!CreateDirectoryW(source, NULL)) return 1;
    GoldenDirectoryMoveResult result = golden_move_directory(source, destination);
    int failed = result != GOLDEN_DIRECTORY_MOVE_DESTINATION_INSIDE_SOURCE ||
        GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES;
    RemoveDirectoryW(source);
    return failed;
}

static int test_directory_move_failure_preserves_source(const wchar_t *directory) {
    wchar_t source[MAX_PATH], destination[MAX_PATH];
    _snwprintf(source, _countof(source), L"%s\\move-fails", directory);
    _snwprintf(destination, _countof(destination), L"%s\\not-created", directory);
    if (!CreateDirectoryW(source, NULL)) return 1;
    MoveContext context = {0, 1u << 1};
    GoldenDirectoryMoveResult result = golden_move_directory_with_move(
        source, destination, controlled_move, &context);
    int failed = result != GOLDEN_DIRECTORY_MOVE_FAILED || context.calls != 1 ||
        GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(destination) != INVALID_FILE_ATTRIBUTES;
    RemoveDirectoryW(source);
    return failed;
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
    int failed = test_checked_paths() ||
        !golden_resource_json_path(old_png, old_json, _countof(old_json)) ||
        !golden_resource_json_path(new_png, new_json, _countof(new_json));
    BYTE png[] = {1, 2, 3, 4};
    Annotation annotation = {0};
    wcscpy(annotation.name, L"before_save");
    annotation.boundary = (RECT){1, 2, 11, 22};
    size_t json_length = 0;
    char *json = golden_document_serialize_utf8(&annotation, 1, &json_length);
    failed = failed || !json || !write_bytes(old_png, png, sizeof(png)) ||
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

    if (!failed) failed = test_json_failure_rolls_back(directory);
    if (!failed) failed = test_rollback_failure_is_reported(directory);
    if (!failed) failed = test_transaction_journal_recovers_split_pair(directory);
    if (!failed) failed = test_first_move_failure_is_reported(directory);
    if (!failed) failed = test_pair_moves_between_directories(directory);
    if (!failed) failed = test_orphan_destination_json_blocks_png_move(directory);
    if (!failed) failed = test_resource_pair_move_undo_redo(directory);
    if (!failed) failed = test_resource_pair_copy(directory);
    if (!failed) failed = test_resource_pair_copy_rolls_back(directory);
    if (!failed) failed = test_resource_pair_replace_undo_redo(directory);
    if (!failed) failed = test_directory_move(directory);
    if (!failed) failed = test_directory_create_undo_redo(directory);
    if (!failed) failed = test_directory_move_rejects_descendant(directory);
    if (!failed) failed = test_directory_move_failure_preserves_source(directory);

    DeleteFileW(old_png); DeleteFileW(old_json);
    DeleteFileW(new_png); DeleteFileW(new_json); DeleteFileW(collision_png);
    RemoveDirectoryW(directory);
    if (failed) return 1;
    puts("All Goldens resource rename tests passed.");
    return 0;
}

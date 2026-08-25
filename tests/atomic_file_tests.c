#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../src/atomic_file.h"

static int failures;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fwprintf(stderr, L"FAIL %S:%d: %S\n", __FILE__, __LINE__, #expression); \
        failures++; \
    } \
} while (0)

static BOOL write_bytes_direct(const wchar_t *path, const void *data,
                               size_t length) {
    FILE *file = _wfopen(path, L"wb");
    if (!file) return FALSE;
    BOOL ok = fwrite(data, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}

static char *read_bytes(const wchar_t *path, size_t *length) {
    FILE *file = _wfopen(path, L"rb");
    if (!file || fseek(file, 0, SEEK_END)) {
        if (file) fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    char *data = (char *)malloc((size_t)size + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    *length = fread(data, 1, (size_t)size, file);
    data[*length] = 0;
    fclose(file);
    return data;
}

static void make_test_directory(wchar_t *directory, size_t capacity) {
    wchar_t temporary[MAX_PATH], seed[MAX_PATH];
    CHECK(GetTempPathW(_countof(temporary), temporary) != 0);
    CHECK(GetTempFileNameW(temporary, L"gaf", 0, seed) != 0);
    DeleteFileW(seed);
    wcsncpy(directory, seed, capacity - 1);
    directory[capacity - 1] = 0;
    CHECK(CreateDirectoryW(directory, NULL));
}

typedef struct {
    wchar_t path[MAX_PATH];
    const char *bytes;
    BOOL result;
    BOOL create_file;
} WriterContext;

static BOOL controlled_writer(const wchar_t *temporary_path, void *opaque) {
    WriterContext *context = (WriterContext *)opaque;
    wcsncpy(context->path, temporary_path, _countof(context->path) - 1);
    context->path[_countof(context->path) - 1] = 0;
    if (context->create_file)
        CHECK(write_bytes_direct(temporary_path, context->bytes,
                                 strlen(context->bytes)));
    return context->result;
}

static void check_contents(const wchar_t *path, const char *expected) {
    size_t length = 0;
    char *actual = read_bytes(path, &length);
    CHECK(actual != NULL);
    CHECK(actual && length == strlen(expected));
    CHECK(actual && !memcmp(actual, expected, length));
    free(actual);
}

static void test_successful_replacement(const wchar_t *directory) {
    wchar_t path[MAX_PATH];
    _snwprintf(path, _countof(path), L"%s\\annotations.json", directory);
    CHECK(write_bytes_direct(path, "old-good-data", 13));
    CHECK(golden_atomic_write_bytes(path, "new-data", 8));
    check_contents(path, "new-data");
    CHECK(golden_atomic_write_bytes(path, "", 0));
    check_contents(path, "");
    DeleteFileW(path);
}

static void test_failed_writer_preserves_destination(const wchar_t *directory) {
    wchar_t path[MAX_PATH];
    _snwprintf(path, _countof(path), L"%s\\preserved.json", directory);
    CHECK(write_bytes_direct(path, "last-known-good", 15));
    WriterContext context = {{0}, "partial", FALSE, TRUE};
    CHECK(!golden_atomic_replace_file(path, controlled_writer, &context));
    check_contents(path, "last-known-good");
    CHECK(context.path[0] != 0);
    CHECK(GetFileAttributesW(context.path) == INVALID_FILE_ATTRIBUTES);
    DeleteFileW(path);
}

static void test_missing_output_preserves_destination(const wchar_t *directory) {
    wchar_t path[MAX_PATH];
    _snwprintf(path, _countof(path), L"%s\\missing-output.png", directory);
    CHECK(write_bytes_direct(path, "original", 8));
    WriterContext context = {{0}, "ignored", TRUE, FALSE};
    CHECK(!golden_atomic_replace_file(path, controlled_writer, &context));
    check_contents(path, "original");
    CHECK(GetFileAttributesW(context.path) == INVALID_FILE_ATTRIBUTES);
    DeleteFileW(path);
}

static void test_failed_new_file_leaves_no_artifact(const wchar_t *directory) {
    wchar_t path[MAX_PATH];
    _snwprintf(path, _countof(path), L"%s\\new-file.json", directory);
    WriterContext context = {{0}, "partial", FALSE, TRUE};
    CHECK(!golden_atomic_replace_file(path, controlled_writer, &context));
    CHECK(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(context.path) == INVALID_FILE_ATTRIBUTES);
}

static void test_writer_uses_destination_directory(const wchar_t *directory) {
    wchar_t path[MAX_PATH];
    _snwprintf(path, _countof(path), L"%s\\same-directory.json", directory);
    WriterContext context = {{0}, "complete", TRUE, TRUE};
    CHECK(golden_atomic_replace_file(path, controlled_writer, &context));
    wchar_t temporary_directory[MAX_PATH];
    wcsncpy(temporary_directory, context.path, _countof(temporary_directory) - 1);
    temporary_directory[_countof(temporary_directory) - 1] = 0;
    wchar_t *slash = wcsrchr(temporary_directory, L'\\');
    CHECK(slash != NULL);
    if (slash) *slash = 0;
    CHECK(!_wcsicmp(temporary_directory, directory));
    check_contents(path, "complete");
    DeleteFileW(path);
}

int main(void) {
    wchar_t directory[MAX_PATH] = L"";
    make_test_directory(directory, _countof(directory));
    test_successful_replacement(directory);
    test_failed_writer_preserves_destination(directory);
    test_missing_output_preserves_destination(directory);
    test_failed_new_file_leaves_no_artifact(directory);
    test_writer_uses_destination_directory(directory);
    CHECK(!golden_atomic_replace_file(NULL, controlled_writer, NULL));
    CHECK(!golden_atomic_replace_file(L"x", NULL, NULL));
    CHECK(!golden_atomic_write_bytes(NULL, "x", 1));
    CHECK(!golden_atomic_write_bytes(L"x", NULL, 1));
    RemoveDirectoryW(directory);
    if (failures) {
        fprintf(stderr, "%d atomic-file test(s) failed.\n", failures);
        return 1;
    }
    puts("All Goldens atomic-file tests passed.");
    return 0;
}

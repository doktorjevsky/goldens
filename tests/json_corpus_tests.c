#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../src/document.h"

static char *read_file(const wchar_t *path, size_t *length) {
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
    size_t received = fread(data, 1, (size_t)size, file);
    BOOL read_ok = received == (size_t)size && !ferror(file);
    BOOL close_ok = fclose(file) == 0;
    if (!read_ok || !close_ok) {
        free(data);
        return NULL;
    }
    data[received] = 0;
    *length = received;
    return data;
}

static BOOL check_case(const wchar_t *path, BOOL expected, BOOL *accepted) {
    static const char prefix[] = "{\"annotations\":[],\"corpus\":";
    static const char suffix[] = "}";
    size_t input_length = 0;
    char *input = read_file(path, &input_length);
    if (!input || input_length > SIZE_MAX - sizeof(prefix) - sizeof(suffix)) {
        free(input);
        return FALSE;
    }
    size_t document_length = sizeof(prefix) - 1 + input_length + sizeof(suffix) - 1;
    char *document = (char *)malloc(document_length);
    if (!document) {
        free(input);
        return FALSE;
    }
    memcpy(document, prefix, sizeof(prefix) - 1);
    memcpy(document + sizeof(prefix) - 1, input, input_length);
    memcpy(document + sizeof(prefix) - 1 + input_length,
           suffix, sizeof(suffix) - 1);
    Annotation annotation = {0};
    int count = 1;
    *accepted = golden_document_parse_utf8(
        document, document_length, &annotation, &count);
    BOOL correct = *accepted == expected && (!*accepted || count == 0);
    free(document);
    free(input);
    return correct;
}

int wmain(int argument_count, wchar_t **arguments) {
    if (argument_count != 2) {
        fwprintf(stderr, L"Usage: json_corpus_tests.exe <test_parsing directory>\n");
        return 2;
    }
    wchar_t pattern[MAX_PATH * 4];
    int pattern_length = _snwprintf(
        pattern, _countof(pattern), L"%ls\\*.json", arguments[1]);
    if (pattern_length < 0 || (size_t)pattern_length >= _countof(pattern))
        return 2;

    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"No JSON corpus files found in %ls.\n", arguments[1]);
        return 2;
    }
    int valid = 0, invalid = 0, implementation_defined = 0, failures = 0;
    do {
        wchar_t category = found.cFileName[0];
        if (category == L'i') {
            ++implementation_defined;
            continue;
        }
        if (category != L'y' && category != L'n') continue;
        wchar_t path[MAX_PATH * 4];
        int path_length = _snwprintf(
            path, _countof(path), L"%ls\\%ls", arguments[1], found.cFileName);
        if (path_length < 0 || (size_t)path_length >= _countof(path)) {
            fwprintf(stderr, L"FAIL path too long: %ls\n", found.cFileName);
            ++failures;
            continue;
        }
        BOOL expected = category == L'y';
        BOOL accepted = FALSE;
        if (!check_case(path, expected, &accepted)) {
            fwprintf(stderr, L"FAIL %ls: expected %ls, got %ls\n",
                found.cFileName, expected ? L"accept" : L"reject",
                accepted ? L"accept" : L"reject");
            ++failures;
        }
        if (expected) ++valid;
        else ++invalid;
    } while (FindNextFileW(search, &found));
    FindClose(search);

    if (valid != 95 || invalid != 188 || implementation_defined != 35) {
        fwprintf(stderr,
            L"FAIL incomplete or different corpus: y=%d n=%d i=%d\n",
            valid, invalid, implementation_defined);
        ++failures;
    }
    if (failures) {
        fwprintf(stderr, L"%d JSON corpus test(s) failed.\n", failures);
        return 1;
    }
    printf("JSONTestSuite passed: %d accepted, %d rejected, %d implementation-defined skipped.\n",
           valid, invalid, implementation_defined);
    return 0;
}

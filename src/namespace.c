#include "namespace.h"

#include "document.h"
#include "resource_ops.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    wchar_t name[128];
    wchar_t png[MAX_PATH * 4];
} NamespaceName;

static void clear_issue(GoldenNamespaceIssue *issue) {
    if (issue) ZeroMemory(issue, sizeof(*issue));
}

static void copy_issue_path(wchar_t *output, size_t capacity,
                            const wchar_t *path) {
    if (!output || !capacity) return;
    if (!path) path = L"";
    wcsncpy(output, path, capacity - 1);
    output[capacity - 1] = 0;
}

static BOOL ends_with_png(const wchar_t *path) {
    size_t length = path ? wcslen(path) : 0;
    return length >= 4 && !_wcsicmp(path + length - 4, L".png");
}

GoldenNamespaceStatus golden_namespace_load_annotations_with_metadata(
    const wchar_t *png_path, Annotation *annotations, int *count,
    GoldenDocumentMetadata *metadata, GoldenNamespaceIssue *issue) {
    clear_issue(issue);
    if (!png_path || !annotations || !count || *count < 0 ||
        *count > MAX_ANNOTATIONS)
        return GOLDEN_NAMESPACE_INVALID_ARGUMENT;
    wchar_t sidecar[MAX_PATH * 4];
    if (!golden_resource_json_path(png_path, sidecar, _countof(sidecar)))
        return GOLDEN_NAMESPACE_PATH_TOO_LONG;
    DWORD attributes = GetFileAttributesW(sidecar);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            *count = 0;
            return GOLDEN_NAMESPACE_OK;
        }
        copy_issue_path(issue ? issue->first_png : NULL,
                        issue ? _countof(issue->first_png) : 0, png_path);
        return GOLDEN_NAMESPACE_INVALID_SIDECAR;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        copy_issue_path(issue ? issue->first_png : NULL,
                        issue ? _countof(issue->first_png) : 0, png_path);
        return GOLDEN_NAMESPACE_INVALID_SIDECAR;
    }

    FILE *file = _wfopen(sidecar, L"rb");
    if (!file) {
        copy_issue_path(issue ? issue->first_png : NULL,
                        issue ? _countof(issue->first_png) : 0, png_path);
        return GOLDEN_NAMESPACE_INVALID_SIDECAR;
    }
    GoldenNamespaceStatus status = GOLDEN_NAMESPACE_INVALID_SIDECAR;
    if (fseek(file, 0, SEEK_END) == 0) {
        errno = 0;
        long length = ftell(file);
        if (length > 0 && length <= 16 * 1024 * 1024 && errno == 0 &&
            fseek(file, 0, SEEK_SET) == 0) {
            char *text = (char *)malloc((size_t)length + 1);
            if (!text) {
                fclose(file);
                return GOLDEN_NAMESPACE_OUT_OF_MEMORY;
            }
            size_t got = fread(text, 1, (size_t)length, file);
            text[got] = 0;
            if (got == (size_t)length &&
                golden_document_parse_utf8_with_metadata(
                    text, got, annotations, count, metadata))
                status = GOLDEN_NAMESPACE_OK;
            free(text);
        }
    }
    fclose(file);
    if (status != GOLDEN_NAMESPACE_OK)
        copy_issue_path(issue ? issue->first_png : NULL,
                        issue ? _countof(issue->first_png) : 0, png_path);
    return status;
}

GoldenNamespaceStatus golden_namespace_load_annotations(
    const wchar_t *png_path, Annotation *annotations, int *count,
    GoldenNamespaceIssue *issue) {
    return golden_namespace_load_annotations_with_metadata(
        png_path, annotations, count, NULL, issue);
}

static GoldenNamespaceStatus append_name(
    NamespaceName **names, size_t *count, size_t *capacity,
    const wchar_t *name, const wchar_t *png, GoldenNamespaceIssue *issue) {
    for (size_t i = 0; i < *count; ++i) {
        if (!_wcsicmp((*names)[i].name, name)) {
            if (issue) {
                copy_issue_path(issue->annotation,
                                _countof(issue->annotation), name);
                copy_issue_path(issue->first_png,
                                _countof(issue->first_png), (*names)[i].png);
                copy_issue_path(issue->second_png,
                                _countof(issue->second_png), png);
            }
            return GOLDEN_NAMESPACE_DUPLICATE;
        }
    }
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 32;
        if (next < *capacity || next > SIZE_MAX / sizeof(**names))
            return GOLDEN_NAMESPACE_OUT_OF_MEMORY;
        NamespaceName *grown = (NamespaceName *)realloc(
            *names, next * sizeof(**names));
        if (!grown) return GOLDEN_NAMESPACE_OUT_OF_MEMORY;
        *names = grown;
        *capacity = next;
    }
    copy_issue_path((*names)[*count].name,
                    _countof((*names)[*count].name), name);
    copy_issue_path((*names)[*count].png,
                    _countof((*names)[*count].png), png);
    ++*count;
    return GOLDEN_NAMESPACE_OK;
}

static BOOL excluded(const wchar_t *path, const wchar_t *first,
                     const wchar_t *second) {
    return (first && !_wcsicmp(path, first)) ||
           (second && !_wcsicmp(path, second));
}

GoldenNamespaceStatus golden_namespace_validate_directory(
    const wchar_t *directory,
    const Annotation *candidate_annotations, int candidate_count,
    const wchar_t *candidate_png,
    const wchar_t *excluded_png,
    const wchar_t *replaced_png,
    GoldenNamespaceIssue *issue) {
    clear_issue(issue);
    if (!directory || !directory[0] || candidate_count < 0 ||
        candidate_count > MAX_ANNOTATIONS ||
        (candidate_count && !candidate_annotations))
        return GOLDEN_NAMESPACE_INVALID_ARGUMENT;

    NamespaceName *names = NULL;
    size_t name_count = 0, name_capacity = 0;
    const wchar_t *candidate_source = candidate_png ? candidate_png : L"";
    GoldenNamespaceStatus status = GOLDEN_NAMESPACE_OK;
    for (int i = 0; i < candidate_count; ++i) {
        if (!golden_annotation_name_valid(candidate_annotations[i].name)) {
            copy_issue_path(issue ? issue->first_png : NULL,
                            issue ? _countof(issue->first_png) : 0,
                            candidate_source);
            status = GOLDEN_NAMESPACE_INVALID_SIDECAR;
            goto done;
        }
        status = append_name(&names, &name_count, &name_capacity,
                             candidate_annotations[i].name,
                             candidate_source, issue);
        if (status != GOLDEN_NAMESPACE_OK) goto done;
    }

    wchar_t pattern[MAX_PATH * 4];
    if (!golden_path_join(directory, L"*", pattern, _countof(pattern))) {
        status = GOLDEN_NAMESPACE_PATH_TOO_LONG;
        goto done;
    }
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        status = error == ERROR_FILE_NOT_FOUND ? GOLDEN_NAMESPACE_OK :
                                                GOLDEN_NAMESPACE_SCAN_FAILED;
        goto done;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !ends_with_png(data.cFileName)) continue;
        wchar_t png[MAX_PATH * 4];
        if (!golden_path_join(directory, data.cFileName,
                              png, _countof(png))) {
            status = GOLDEN_NAMESPACE_PATH_TOO_LONG;
            break;
        }
        if (excluded(png, excluded_png, replaced_png)) continue;
        Annotation annotations[MAX_ANNOTATIONS];
        int count = MAX_ANNOTATIONS;
        status = golden_namespace_load_annotations(
            png, annotations, &count, issue);
        if (status != GOLDEN_NAMESPACE_OK) break;
        for (int i = 0; i < count; ++i) {
            status = append_name(&names, &name_count, &name_capacity,
                                 annotations[i].name, png, issue);
            if (status != GOLDEN_NAMESPACE_OK) break;
        }
    } while (status == GOLDEN_NAMESPACE_OK && FindNextFileW(find, &data));
    if (status == GOLDEN_NAMESPACE_OK && GetLastError() != ERROR_NO_MORE_FILES)
        status = GOLDEN_NAMESPACE_SCAN_FAILED;
    FindClose(find);

done:
    free(names);
    return status;
}

GoldenNamespaceStatus golden_namespace_find_name(
    const wchar_t *directory, const wchar_t *excluded_png,
    const wchar_t *name, BOOL *found, GoldenNamespaceIssue *issue) {
    clear_issue(issue);
    if (found) *found = FALSE;
    if (!directory || !directory[0] || !name || !found)
        return GOLDEN_NAMESPACE_INVALID_ARGUMENT;
    wchar_t pattern[MAX_PATH * 4];
    if (!golden_path_join(directory, L"*", pattern, _countof(pattern)))
        return GOLDEN_NAMESPACE_PATH_TOO_LONG;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? GOLDEN_NAMESPACE_OK :
                                              GOLDEN_NAMESPACE_SCAN_FAILED;
    }
    GoldenNamespaceStatus status = GOLDEN_NAMESPACE_OK;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !ends_with_png(data.cFileName)) continue;
        wchar_t png[MAX_PATH * 4];
        if (!golden_path_join(directory, data.cFileName,
                              png, _countof(png))) {
            status = GOLDEN_NAMESPACE_PATH_TOO_LONG;
            break;
        }
        if (excluded_png && !_wcsicmp(png, excluded_png)) continue;
        Annotation annotations[MAX_ANNOTATIONS];
        int count = MAX_ANNOTATIONS;
        status = golden_namespace_load_annotations(
            png, annotations, &count, issue);
        if (status != GOLDEN_NAMESPACE_OK) break;
        if (golden_name_exists(annotations, count, name, -1)) {
            *found = TRUE;
            if (issue) copy_issue_path(issue->first_png,
                                       _countof(issue->first_png), png);
            break;
        }
    } while (FindNextFileW(find, &data));
    if (status == GOLDEN_NAMESPACE_OK && !*found &&
        GetLastError() != ERROR_NO_MORE_FILES)
        status = GOLDEN_NAMESPACE_SCAN_FAILED;
    FindClose(find);
    return status;
}

GoldenNamespaceStatus golden_namespace_mark_collisions(
    const wchar_t *directory, const wchar_t *excluded_png,
    const Annotation *annotations, int count,
    BOOL *collisions, GoldenNamespaceIssue *issue) {
    clear_issue(issue);
    if (!directory || !directory[0] || count < 0 ||
        count > MAX_ANNOTATIONS || (count && (!annotations || !collisions)))
        return GOLDEN_NAMESPACE_INVALID_ARGUMENT;
    for (int i = 0; i < count; ++i) collisions[i] = FALSE;
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (!_wcsicmp(annotations[i].name, annotations[j].name))
                collisions[i] = collisions[j] = TRUE;
        }
    }

    wchar_t pattern[MAX_PATH * 4];
    if (!golden_path_join(directory, L"*", pattern, _countof(pattern)))
        return GOLDEN_NAMESPACE_PATH_TOO_LONG;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? GOLDEN_NAMESPACE_OK :
                                              GOLDEN_NAMESPACE_SCAN_FAILED;
    }
    GoldenNamespaceStatus status = GOLDEN_NAMESPACE_OK;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            !ends_with_png(data.cFileName)) continue;
        wchar_t png[MAX_PATH * 4];
        if (!golden_path_join(directory, data.cFileName,
                              png, _countof(png))) {
            status = GOLDEN_NAMESPACE_PATH_TOO_LONG;
            break;
        }
        if (excluded_png && !_wcsicmp(png, excluded_png)) continue;
        Annotation other[MAX_ANNOTATIONS];
        int other_count = MAX_ANNOTATIONS;
        status = golden_namespace_load_annotations(
            png, other, &other_count, issue);
        if (status != GOLDEN_NAMESPACE_OK) break;
        for (int i = 0; i < count; ++i) {
            if (golden_name_exists(other, other_count,
                                   annotations[i].name, -1))
                collisions[i] = TRUE;
        }
    } while (FindNextFileW(find, &data));
    if (status == GOLDEN_NAMESPACE_OK && GetLastError() != ERROR_NO_MORE_FILES)
        status = GOLDEN_NAMESPACE_SCAN_FAILED;
    FindClose(find);
    return status;
}

GoldenNamespaceStatus golden_namespace_validate_tree(
    const wchar_t *root, GoldenNamespaceIssue *issue) {
    if (!root || !root[0]) return GOLDEN_NAMESPACE_INVALID_ARGUMENT;
    GoldenNamespaceStatus status = golden_namespace_validate_directory(
        root, NULL, 0, NULL, NULL, NULL, issue);
    if (status != GOLDEN_NAMESPACE_OK) return status;

    wchar_t pattern[MAX_PATH * 4];
    if (!golden_path_join(root, L"*", pattern, _countof(pattern)))
        return GOLDEN_NAMESPACE_PATH_TOO_LONG;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? GOLDEN_NAMESPACE_OK :
                                              GOLDEN_NAMESPACE_SCAN_FAILED;
    }
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN |
             FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !wcscmp(data.cFileName, L".") ||
            !wcscmp(data.cFileName, L"..")) continue;
        wchar_t child[MAX_PATH * 4];
        if (!golden_path_join(root, data.cFileName,
                              child, _countof(child))) {
            status = GOLDEN_NAMESPACE_PATH_TOO_LONG;
            break;
        }
        status = golden_namespace_validate_tree(child, issue);
    } while (status == GOLDEN_NAMESPACE_OK && FindNextFileW(find, &data));
    if (status == GOLDEN_NAMESPACE_OK && GetLastError() != ERROR_NO_MORE_FILES)
        status = GOLDEN_NAMESPACE_SCAN_FAILED;
    FindClose(find);
    return status;
}

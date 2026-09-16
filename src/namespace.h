#ifndef GOLDENS_NAMESPACE_H
#define GOLDENS_NAMESPACE_H

#include <windows.h>
#include <stddef.h>

#include "document.h"
#include "model.h"

typedef enum {
    GOLDEN_NAMESPACE_OK,
    GOLDEN_NAMESPACE_INVALID_ARGUMENT,
    GOLDEN_NAMESPACE_PATH_TOO_LONG,
    GOLDEN_NAMESPACE_SCAN_FAILED,
    GOLDEN_NAMESPACE_INVALID_SIDECAR,
    GOLDEN_NAMESPACE_OUT_OF_MEMORY,
    GOLDEN_NAMESPACE_DUPLICATE
} GoldenNamespaceStatus;

typedef struct {
    wchar_t annotation[128];
    wchar_t first_png[MAX_PATH * 4];
    wchar_t second_png[MAX_PATH * 4];
} GoldenNamespaceIssue;

GoldenNamespaceStatus golden_namespace_load_annotations(
    const wchar_t *png_path, Annotation *annotations, int *count,
    GoldenNamespaceIssue *issue);
GoldenNamespaceStatus golden_namespace_load_annotations_with_metadata(
    const wchar_t *png_path, Annotation *annotations, int *count,
    GoldenDocumentMetadata *metadata, GoldenNamespaceIssue *issue);

GoldenNamespaceStatus golden_namespace_validate_directory(
    const wchar_t *directory,
    const Annotation *candidate_annotations, int candidate_count,
    const wchar_t *candidate_png,
    const wchar_t *excluded_png,
    const wchar_t *replaced_png,
    GoldenNamespaceIssue *issue);

GoldenNamespaceStatus golden_namespace_find_name(
    const wchar_t *directory, const wchar_t *excluded_png,
    const wchar_t *name, BOOL *found, GoldenNamespaceIssue *issue);

GoldenNamespaceStatus golden_namespace_mark_collisions(
    const wchar_t *directory, const wchar_t *excluded_png,
    const Annotation *annotations, int count,
    BOOL *collisions, GoldenNamespaceIssue *issue);

GoldenNamespaceStatus golden_namespace_validate_tree(
    const wchar_t *root, GoldenNamespaceIssue *issue);

#endif

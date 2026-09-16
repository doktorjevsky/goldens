#ifndef GOLDENS_DOCUMENT_H
#define GOLDENS_DOCUMENT_H

#include "model.h"

#include <stddef.h>

typedef struct {
    BOOL has_scale;
    double scale;
} GoldenDocumentMetadata;

BOOL golden_document_parse_utf8(const char *text, size_t length,
                                Annotation *items, int *count);
BOOL golden_document_parse_utf8_with_metadata(
    const char *text, size_t length, Annotation *items, int *count,
    GoldenDocumentMetadata *metadata);
char *golden_document_serialize_utf8(const Annotation *items, int count,
                                     size_t *length);
char *golden_document_serialize_utf8_with_metadata(
    const Annotation *items, int count,
    const GoldenDocumentMetadata *metadata, size_t *length);

#endif

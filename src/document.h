#ifndef GOLDENS_DOCUMENT_H
#define GOLDENS_DOCUMENT_H

#include "model.h"

#include <stddef.h>

BOOL golden_document_parse_utf8(const char *text, size_t length,
                                Annotation *items, int *count);
char *golden_document_serialize_utf8(const Annotation *items, int count,
                                     size_t *length);

#endif


#ifndef GOLDENS_ATOMIC_FILE_H
#define GOLDENS_ATOMIC_FILE_H

#include <windows.h>
#include <stddef.h>

typedef BOOL (*GoldenAtomicFileWriter)(const wchar_t *temporary_path,
                                       void *context);

BOOL golden_atomic_replace_file(const wchar_t *destination,
                                GoldenAtomicFileWriter writer,
                                void *context);
BOOL golden_atomic_write_bytes(const wchar_t *destination,
                               const void *data, size_t length);
BOOL golden_atomic_copy_file(const wchar_t *source,
                             const wchar_t *destination);

#endif

#include "atomic_file.h"

#include <stdint.h>
#include <wchar.h>

typedef struct {
    const BYTE *data;
    size_t length;
} ByteWriterContext;

static volatile LONG temporary_counter;

static BOOL make_temporary_path(const wchar_t *destination,
                                wchar_t *temporary, size_t capacity) {
    for (int attempt = 0; attempt < 128; ++attempt) {
        DWORD process = GetCurrentProcessId();
        LONG sequence = InterlockedIncrement(&temporary_counter);
        int length = _snwprintf(temporary, capacity,
            L"%s.goldens-%08lx-%08lx.tmp", destination,
            (unsigned long)process, (unsigned long)sequence);
        if (length < 0 || (size_t)length >= capacity) return FALSE;
        HANDLE reservation = CreateFileW(temporary, GENERIC_WRITE, 0, NULL,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (reservation != INVALID_HANDLE_VALUE) {
            CloseHandle(reservation);
            if (DeleteFileW(temporary)) return TRUE;
            return FALSE;
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    }
    return FALSE;
}

static BOOL flush_file(const wchar_t *path) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    BOOL flushed = FlushFileBuffers(file);
    BOOL closed = CloseHandle(file);
    return flushed && closed;
}

BOOL golden_atomic_replace_file(const wchar_t *destination,
                                GoldenAtomicFileWriter writer,
                                void *context) {
    if (!destination || !destination[0] || !writer) return FALSE;
    wchar_t temporary[MAX_PATH * 4];
    if (!make_temporary_path(destination, temporary, _countof(temporary)))
        return FALSE;

    BOOL written = writer(temporary, context);
    DWORD attributes = written ? GetFileAttributesW(temporary) :
                                 INVALID_FILE_ATTRIBUTES;
    if (!written || attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) || !flush_file(temporary)) {
        DeleteFileW(temporary);
        return FALSE;
    }
    if (!MoveFileExW(temporary, destination,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary);
        return FALSE;
    }
    return TRUE;
}

static BOOL write_bytes(const wchar_t *temporary_path, void *opaque) {
    ByteWriterContext *context = (ByteWriterContext *)opaque;
    HANDLE file = CreateFileW(temporary_path, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    size_t remaining = context->length;
    const BYTE *cursor = context->data;
    BOOL written = TRUE;
    while (remaining) {
        DWORD chunk = remaining > UINT32_MAX ? UINT32_MAX : (DWORD)remaining;
        DWORD completed = 0;
        if (!WriteFile(file, cursor, chunk, &completed, NULL) ||
            completed != chunk) {
            written = FALSE;
            break;
        }
        cursor += completed;
        remaining -= completed;
    }
    BOOL closed = CloseHandle(file);
    return written && closed;
}

BOOL golden_atomic_write_bytes(const wchar_t *destination,
                               const void *data, size_t length) {
    if (!destination || (length && !data)) return FALSE;
    ByteWriterContext context = {(const BYTE *)data, length};
    return golden_atomic_replace_file(destination, write_bytes, &context);
}

static BOOL copy_file_writer(const wchar_t *temporary_path, void *context) {
    return CopyFileW((const wchar_t *)context, temporary_path, FALSE);
}

BOOL golden_atomic_copy_file(const wchar_t *source,
                             const wchar_t *destination) {
    if (!source || !source[0]) return FALSE;
    return golden_atomic_replace_file(destination, copy_file_writer,
                                      (void *)source);
}

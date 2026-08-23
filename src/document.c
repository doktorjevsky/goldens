#include "document.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} TextBuffer;

static const char *skip_space(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static const char *find_key(const char *start, const char *end, const char *key) {
    size_t length = strlen(key);
    for (const char *p = start; p + length + 2 < end; ++p)
        if (*p == '"' && !memcmp(p + 1, key, length) && p[length + 1] == '"')
            return p + length + 2;
    return NULL;
}

static BOOL parse_long(const char *start, const char *end, const char *key, LONG *out) {
    const char *p = find_key(start, end, key);
    if (!p || !(p = strchr(p, ':')) || p >= end) return FALSE;
    char *tail;
    long value = strtol(skip_space(p + 1), &tail, 10);
    if (tail == p + 1 || tail > end) return FALSE;
    *out = value;
    return TRUE;
}

static BOOL parse_double(const char *start, const char *end, const char *key, double *out) {
    const char *p = find_key(start, end, key);
    if (!p || !(p = strchr(p, ':')) || p >= end) return FALSE;
    char *tail;
    double value = strtod(skip_space(p + 1), &tail);
    if (tail == p + 1 || tail > end) return FALSE;
    *out = value;
    return TRUE;
}

static BOOL parse_name(const char *key_end, const char *end, wchar_t *out, size_t capacity) {
    const char *p = strchr(key_end, ':');
    if (!p || p >= end || *(p = skip_space(p + 1)) != '"') return FALSE;
    ++p;
    char utf8[512];
    int length = 0;
    while (p < end && *p != '"' && length < (int)sizeof(utf8) - 1) {
        if (*p == '\\' && p + 1 < end) {
            ++p;
            if (*p == 'n') utf8[length++] = '\n';
            else if (*p == 'r') utf8[length++] = '\r';
            else if (*p == 't') utf8[length++] = '\t';
            else utf8[length++] = *p;
            ++p;
        } else utf8[length++] = *p++;
    }
    if (p >= end || *p != '"') return FALSE;
    int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, length, NULL, 0);
    if (wide_length <= 0 || (size_t)wide_length >= capacity) return FALSE;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, length, out, wide_length);
    out[wide_length] = 0;
    return TRUE;
}

BOOL golden_document_parse_utf8(const char *text, size_t length,
                                Annotation *items, int *count) {
    if (!text || !items || !count || *count < 0) return FALSE;
    int capacity = *count, parsed = 0;
    const char *end = text + length, *cursor = text;
    while (parsed < capacity) {
        const char *name = find_key(cursor, end, "name");
        if (!name) break;
        const char *next = find_key(name, end, "name");
        const char *section_end = next ? next - 6 : end;
        Annotation annotation = {0};
        LONG x, y, width, height;
        if (!parse_name(name, section_end, annotation.name, 128) ||
            !parse_long(name, section_end, "x", &x) ||
            !parse_long(name, section_end, "y", &y) ||
            !parse_long(name, section_end, "width", &width) ||
            !parse_long(name, section_end, "height", &height) || width <= 0 || height <= 0)
            return FALSE;
        annotation.boundary = (RECT){x, y, x + width, y + height};
        const char *click = find_key(name, section_end, "click");
        if (click) {
            if (!parse_double(click, section_end, "x", &annotation.click_x) ||
                !parse_double(click, section_end, "y", &annotation.click_y) ||
                annotation.click_x < 0.0 || annotation.click_x > 1.0 ||
                annotation.click_y < 0.0 || annotation.click_y > 1.0) return FALSE;
            annotation.has_click = TRUE;
        }
        if (golden_name_exists(items, parsed, annotation.name, -1)) return FALSE;
        items[parsed++] = annotation;
        cursor = next ? next - 6 : end;
    }
    *count = parsed;
    return TRUE;
}

static BOOL reserve(TextBuffer *buffer, size_t extra) {
    if (buffer->length + extra + 1 <= buffer->capacity) return TRUE;
    size_t capacity = buffer->capacity ? buffer->capacity : 512;
    while (capacity < buffer->length + extra + 1) capacity *= 2;
    char *data = (char *)realloc(buffer->data, capacity);
    if (!data) return FALSE;
    buffer->data = data;
    buffer->capacity = capacity;
    return TRUE;
}

static BOOL append_bytes(TextBuffer *buffer, const char *value, size_t length) {
    if (!reserve(buffer, length)) return FALSE;
    memcpy(buffer->data + buffer->length, value, length);
    buffer->length += length;
    buffer->data[buffer->length] = 0;
    return TRUE;
}

static BOOL append_text(TextBuffer *buffer, const char *value) {
    return append_bytes(buffer, value, strlen(value));
}

static BOOL append_format(TextBuffer *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int length = _vscprintf(format, args);
    va_end(args);
    if (length < 0 || !reserve(buffer, (size_t)length)) return FALSE;
    va_start(args, format);
    vsnprintf(buffer->data + buffer->length, buffer->capacity - buffer->length, format, args);
    va_end(args);
    buffer->length += (size_t)length;
    return TRUE;
}

static BOOL append_name(TextBuffer *buffer, const wchar_t *name) {
    int length = WideCharToMultiByte(CP_UTF8, 0, name, -1, NULL, 0, NULL, NULL);
    char *utf8 = length ? (char *)malloc((size_t)length) : NULL;
    if (!utf8) return FALSE;
    WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, length, NULL, NULL);
    BOOL ok = TRUE;
    for (const unsigned char *p = (unsigned char *)utf8; ok && *p; ++p) {
        if (*p == '"' || *p == '\\') { char escaped[2] = {'\\', (char)*p}; ok = append_bytes(buffer, escaped, 2); }
        else if (*p == '\n') ok = append_text(buffer, "\\n");
        else if (*p == '\r') ok = append_text(buffer, "\\r");
        else if (*p == '\t') ok = append_text(buffer, "\\t");
        else ok = append_bytes(buffer, (char *)p, 1);
    }
    free(utf8);
    return ok;
}

char *golden_document_serialize_utf8(const Annotation *items, int count, size_t *length) {
    TextBuffer buffer = {0};
    if (!append_text(&buffer, "{\n  \"annotations\": [")) goto fail;
    for (int i = 0; i < count; ++i) {
        const Annotation *annotation = &items[i];
        if (!append_format(&buffer, "%s\n    {\n      \"name\": \"", i ? "," : "") ||
            !append_name(&buffer, annotation->name) ||
            !append_format(&buffer,
                "\",\n      \"boundary\": { \"x\": %ld, \"y\": %ld, \"width\": %ld, \"height\": %ld }",
                annotation->boundary.left, annotation->boundary.top,
                annotation->boundary.right - annotation->boundary.left,
                annotation->boundary.bottom - annotation->boundary.top)) goto fail;
        if (annotation->has_click && !append_format(&buffer,
            ",\n      \"click\": { \"x\": %.9g, \"y\": %.9g }",
            annotation->click_x, annotation->click_y)) goto fail;
        if (!append_text(&buffer, "\n    }")) goto fail;
    }
    if (!append_text(&buffer, "\n  ]\n}\n")) goto fail;
    if (length) *length = buffer.length;
    return buffer.data;
fail:
    free(buffer.data);
    return NULL;
}

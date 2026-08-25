#include "document.h"

#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} TextBuffer;

typedef struct {
    const char *current;
    const char *end;
    int depth;
} JsonReader;

static void json_skip_space(JsonReader *reader) {
    while (reader->current < reader->end &&
           (*reader->current == ' ' || *reader->current == '\t' ||
            *reader->current == '\r' || *reader->current == '\n'))
        ++reader->current;
}

static BOOL json_consume(JsonReader *reader, char expected) {
    json_skip_space(reader);
    if (reader->current >= reader->end || *reader->current != expected)
        return FALSE;
    ++reader->current;
    return TRUE;
}

static int hex_digit(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static BOOL json_emit_byte(char *output, size_t capacity, size_t *length,
                           unsigned char value) {
    if (output) {
        if (*length + 1 >= capacity) return FALSE;
        output[*length] = (char)value;
    }
    ++*length;
    return TRUE;
}

static BOOL json_emit_codepoint(char *output, size_t capacity, size_t *length,
                                unsigned codepoint) {
    if (codepoint <= 0x7f)
        return json_emit_byte(output, capacity, length, (unsigned char)codepoint);
    if (codepoint <= 0x7ff)
        return json_emit_byte(output, capacity, length,
                              (unsigned char)(0xc0 | (codepoint >> 6))) &&
               json_emit_byte(output, capacity, length,
                              (unsigned char)(0x80 | (codepoint & 0x3f)));
    if (codepoint <= 0xffff)
        return json_emit_byte(output, capacity, length,
                              (unsigned char)(0xe0 | (codepoint >> 12))) &&
               json_emit_byte(output, capacity, length,
                              (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f))) &&
               json_emit_byte(output, capacity, length,
                              (unsigned char)(0x80 | (codepoint & 0x3f)));
    if (codepoint <= 0x10ffff)
        return json_emit_byte(output, capacity, length,
                              (unsigned char)(0xf0 | (codepoint >> 18))) &&
               json_emit_byte(output, capacity, length,
                              (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f))) &&
               json_emit_byte(output, capacity, length,
                              (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f))) &&
               json_emit_byte(output, capacity, length,
                              (unsigned char)(0x80 | (codepoint & 0x3f)));
    return FALSE;
}

static BOOL json_read_hex4(JsonReader *reader, unsigned *value) {
    if ((size_t)(reader->end - reader->current) < 4) return FALSE;
    unsigned result = 0;
    for (int i = 0; i < 4; ++i) {
        int digit = hex_digit((unsigned char)reader->current[i]);
        if (digit < 0) return FALSE;
        result = result * 16 + (unsigned)digit;
    }
    reader->current += 4;
    *value = result;
    return TRUE;
}

static BOOL json_copy_raw_utf8(JsonReader *reader, char *output,
                               size_t capacity, size_t *length) {
    const unsigned char *p = (const unsigned char *)reader->current;
    size_t remaining = (size_t)(reader->end - reader->current);
    unsigned count;
    if (p[0] >= 0xc2 && p[0] <= 0xdf) count = 2;
    else if (p[0] >= 0xe0 && p[0] <= 0xef) count = 3;
    else if (p[0] >= 0xf0 && p[0] <= 0xf4) count = 4;
    else return FALSE;
    if (remaining < count) return FALSE;
    for (unsigned i = 1; i < count; ++i)
        if ((p[i] & 0xc0) != 0x80) return FALSE;
    if ((p[0] == 0xe0 && p[1] < 0xa0) ||
        (p[0] == 0xed && p[1] >= 0xa0) ||
        (p[0] == 0xf0 && p[1] < 0x90) ||
        (p[0] == 0xf4 && p[1] > 0x8f)) return FALSE;
    for (unsigned i = 0; i < count; ++i)
        if (!json_emit_byte(output, capacity, length, p[i])) return FALSE;
    reader->current += count;
    return TRUE;
}

static BOOL json_parse_string(JsonReader *reader, char *output,
                              size_t capacity, size_t *output_length) {
    json_skip_space(reader);
    if (reader->current >= reader->end || *reader->current++ != '"') return FALSE;
    size_t length = 0;
    while (reader->current < reader->end) {
        unsigned char value = (unsigned char)*reader->current++;
        if (value == '"') {
            if (output) output[length] = 0;
            if (output_length) *output_length = length;
            return TRUE;
        }
        if (value < 0x20) return FALSE;
        if (value >= 0x80) {
            --reader->current;
            if (!json_copy_raw_utf8(reader, output, capacity, &length)) return FALSE;
            continue;
        }
        if (value != '\\') {
            if (!json_emit_byte(output, capacity, &length, value)) return FALSE;
            continue;
        }
        if (reader->current >= reader->end) return FALSE;
        unsigned char escaped = (unsigned char)*reader->current++;
        if (escaped == '"' || escaped == '\\' || escaped == '/') value = escaped;
        else if (escaped == 'b') value = '\b';
        else if (escaped == 'f') value = '\f';
        else if (escaped == 'n') value = '\n';
        else if (escaped == 'r') value = '\r';
        else if (escaped == 't') value = '\t';
        else if (escaped == 'u') {
            unsigned codepoint;
            if (!json_read_hex4(reader, &codepoint)) return FALSE;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                if ((size_t)(reader->end - reader->current) < 6 ||
                    reader->current[0] != '\\' || reader->current[1] != 'u')
                    return FALSE;
                reader->current += 2;
                unsigned low;
                if (!json_read_hex4(reader, &low) || low < 0xdc00 || low > 0xdfff)
                    return FALSE;
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
                            (low - 0xdc00);
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) return FALSE;
            if (!json_emit_codepoint(output, capacity, &length, codepoint)) return FALSE;
            continue;
        } else return FALSE;
        if (!json_emit_byte(output, capacity, &length, value)) return FALSE;
    }
    return FALSE;
}

static BOOL json_parse_key(JsonReader *reader, char *key, size_t capacity) {
    if (!key || !capacity) return FALSE;
    JsonReader parsed = *reader;
    size_t length = 0;
    if (!json_parse_string(&parsed, NULL, 0, &length)) return FALSE;
    if (length >= capacity) {
        *reader = parsed;
        key[0] = 0;
        return TRUE;
    }
    if (!json_parse_string(reader, key, capacity, &length)) return FALSE;
    if (memchr(key, 0, length)) key[0] = 0;
    return TRUE;
}

static BOOL json_number_token(JsonReader *reader, const char **start,
                              size_t *length, BOOL *integer) {
    json_skip_space(reader);
    const char *p = reader->current;
    if (p < reader->end && *p == '-') ++p;
    if (p >= reader->end) return FALSE;
    if (*p == '0') {
        ++p;
        if (p < reader->end && *p >= '0' && *p <= '9') return FALSE;
    } else {
        if (*p < '1' || *p > '9') return FALSE;
        do { ++p; } while (p < reader->end && *p >= '0' && *p <= '9');
    }
    BOOL whole = TRUE;
    if (p < reader->end && *p == '.') {
        whole = FALSE;
        ++p;
        if (p >= reader->end || *p < '0' || *p > '9') return FALSE;
        do { ++p; } while (p < reader->end && *p >= '0' && *p <= '9');
    }
    if (p < reader->end && (*p == 'e' || *p == 'E')) {
        whole = FALSE;
        ++p;
        if (p < reader->end && (*p == '+' || *p == '-')) ++p;
        if (p >= reader->end || *p < '0' || *p > '9') return FALSE;
        do { ++p; } while (p < reader->end && *p >= '0' && *p <= '9');
    }
    *start = reader->current;
    *length = (size_t)(p - reader->current);
    *integer = whole;
    reader->current = p;
    return TRUE;
}

static BOOL json_parse_long(JsonReader *reader, LONG *output) {
    const char *start;
    size_t length;
    BOOL integer;
    if (!json_number_token(reader, &start, &length, &integer) || !integer ||
        !length || length >= 64) return FALSE;
    char number[64];
    memcpy(number, start, length);
    number[length] = 0;
    errno = 0;
    char *tail;
    long value = strtol(number, &tail, 10);
    if (errno == ERANGE || tail != number + length) return FALSE;
    *output = value;
    return TRUE;
}

static BOOL json_parse_double(JsonReader *reader, double *output) {
    const char *start;
    size_t length;
    BOOL integer;
    if (!json_number_token(reader, &start, &length, &integer) ||
        !length || length >= 128) return FALSE;
    char number[128];
    memcpy(number, start, length);
    number[length] = 0;
    errno = 0;
    char *tail;
    double value = strtod(number, &tail);
    if (errno == ERANGE || tail != number + length || !isfinite(value)) return FALSE;
    *output = value;
    return TRUE;
}

static BOOL json_skip_value(JsonReader *reader);

static BOOL json_skip_compound(JsonReader *reader, char open, char close) {
    if (++reader->depth > 64 || !json_consume(reader, open)) return FALSE;
    json_skip_space(reader);
    if (reader->current < reader->end && *reader->current == close) {
        ++reader->current;
        --reader->depth;
        return TRUE;
    }
    for (;;) {
        if (open == '{') {
            if (!json_parse_string(reader, NULL, 0, NULL) ||
                !json_consume(reader, ':')) return FALSE;
        }
        if (!json_skip_value(reader)) return FALSE;
        json_skip_space(reader);
        if (reader->current < reader->end && *reader->current == close) {
            ++reader->current;
            --reader->depth;
            return TRUE;
        }
        if (!json_consume(reader, ',')) return FALSE;
    }
}

static BOOL json_skip_value(JsonReader *reader) {
    json_skip_space(reader);
    if (reader->current >= reader->end) return FALSE;
    if (*reader->current == '"') return json_parse_string(reader, NULL, 0, NULL);
    if (*reader->current == '{') return json_skip_compound(reader, '{', '}');
    if (*reader->current == '[') return json_skip_compound(reader, '[', ']');
    const char *literal = NULL;
    size_t literal_length = 0;
    if (*reader->current == 't') { literal = "true"; literal_length = 4; }
    else if (*reader->current == 'f') { literal = "false"; literal_length = 5; }
    else if (*reader->current == 'n') { literal = "null"; literal_length = 4; }
    if (literal) {
        if ((size_t)(reader->end - reader->current) < literal_length ||
            memcmp(reader->current, literal, literal_length)) return FALSE;
        reader->current += literal_length;
        return TRUE;
    }
    const char *start;
    size_t length;
    BOOL integer;
    return json_number_token(reader, &start, &length, &integer);
}

static BOOL json_parse_name(JsonReader *reader, wchar_t *output,
                            size_t capacity) {
    char utf8[512];
    size_t length = 0;
    if (!json_parse_string(reader, utf8, sizeof(utf8), &length) || !length ||
        memchr(utf8, 0, length)) return FALSE;
    int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        utf8, (int)length, NULL, 0);
    if (wide_length <= 0 || (size_t)wide_length >= capacity) return FALSE;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)length,
                            output, wide_length) != wide_length) return FALSE;
    output[wide_length] = 0;
    return TRUE;
}

static BOOL json_parse_boundary(JsonReader *reader, RECT *boundary) {
    if (!json_consume(reader, '{')) return FALSE;
    LONG x = 0, y = 0, width = 0, height = 0;
    unsigned seen = 0;
    json_skip_space(reader);
    if (reader->current < reader->end && *reader->current == '}') return FALSE;
    for (;;) {
        char key[128];
        if (!json_parse_key(reader, key, sizeof(key)) ||
            !json_consume(reader, ':')) return FALSE;
        unsigned bit = !strcmp(key, "x") ? 1u : !strcmp(key, "y") ? 2u :
                       !strcmp(key, "width") ? 4u :
                       !strcmp(key, "height") ? 8u : 0u;
        if (bit && (seen & bit)) return FALSE;
        if (bit) {
            LONG *destination = bit == 1 ? &x : bit == 2 ? &y :
                                bit == 4 ? &width : &height;
            if (!json_parse_long(reader, destination)) return FALSE;
            seen |= bit;
        } else if (!json_skip_value(reader)) return FALSE;
        json_skip_space(reader);
        if (reader->current < reader->end && *reader->current == '}') {
            ++reader->current;
            break;
        }
        if (!json_consume(reader, ',')) return FALSE;
    }
    LONGLONG right = (LONGLONG)x + width;
    LONGLONG bottom = (LONGLONG)y + height;
    if (seen != 15 || width <= 0 || height <= 0 ||
        right < LONG_MIN || right > LONG_MAX ||
        bottom < LONG_MIN || bottom > LONG_MAX) return FALSE;
    *boundary = (RECT){x, y, (LONG)right, (LONG)bottom};
    return TRUE;
}

static BOOL json_parse_click(JsonReader *reader, Annotation *annotation) {
    if (!json_consume(reader, '{')) return FALSE;
    double x = 0.0, y = 0.0;
    unsigned seen = 0;
    json_skip_space(reader);
    if (reader->current < reader->end && *reader->current == '}') return FALSE;
    for (;;) {
        char key[128];
        if (!json_parse_key(reader, key, sizeof(key)) ||
            !json_consume(reader, ':')) return FALSE;
        unsigned bit = !strcmp(key, "x") ? 1u : !strcmp(key, "y") ? 2u : 0u;
        if (bit && (seen & bit)) return FALSE;
        if (bit) {
            double *destination = bit == 1 ? &x : &y;
            if (!json_parse_double(reader, destination)) return FALSE;
            seen |= bit;
        } else if (!json_skip_value(reader)) return FALSE;
        json_skip_space(reader);
        if (reader->current < reader->end && *reader->current == '}') {
            ++reader->current;
            break;
        }
        if (!json_consume(reader, ',')) return FALSE;
    }
    if (seen != 3 || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0)
        return FALSE;
    annotation->has_click = TRUE;
    annotation->click_x = x;
    annotation->click_y = y;
    return TRUE;
}

static BOOL json_parse_annotation(JsonReader *reader, Annotation *annotation) {
    if (!json_consume(reader, '{')) return FALSE;
    unsigned seen = 0;
    json_skip_space(reader);
    if (reader->current < reader->end && *reader->current == '}') return FALSE;
    for (;;) {
        char key[128];
        if (!json_parse_key(reader, key, sizeof(key)) ||
            !json_consume(reader, ':')) return FALSE;
        unsigned bit = !strcmp(key, "name") ? 1u :
                       !strcmp(key, "boundary") ? 2u :
                       !strcmp(key, "click") ? 4u : 0u;
        if (bit && (seen & bit)) return FALSE;
        if (bit == 1 && !json_parse_name(reader, annotation->name,
                                        _countof(annotation->name))) return FALSE;
        if (bit == 2 && !json_parse_boundary(reader, &annotation->boundary)) return FALSE;
        if (bit == 4 && !json_parse_click(reader, annotation)) return FALSE;
        if (!bit && !json_skip_value(reader)) return FALSE;
        seen |= bit;
        json_skip_space(reader);
        if (reader->current < reader->end && *reader->current == '}') {
            ++reader->current;
            break;
        }
        if (!json_consume(reader, ',')) return FALSE;
    }
    return (seen & 3u) == 3u;
}

static BOOL json_parse_annotations(JsonReader *reader, Annotation *items,
                                   int capacity, int *count) {
    if (!json_consume(reader, '[')) return FALSE;
    int parsed = 0;
    json_skip_space(reader);
    if (reader->current < reader->end && *reader->current == ']') {
        ++reader->current;
        *count = 0;
        return TRUE;
    }
    for (;;) {
        if (parsed >= capacity) return FALSE;
        Annotation annotation = {0};
        if (!json_parse_annotation(reader, &annotation) ||
            golden_name_exists(items, parsed, annotation.name, -1)) return FALSE;
        items[parsed++] = annotation;
        json_skip_space(reader);
        if (reader->current < reader->end && *reader->current == ']') {
            ++reader->current;
            *count = parsed;
            return TRUE;
        }
        if (!json_consume(reader, ',')) return FALSE;
    }
}

BOOL golden_document_parse_utf8(const char *text, size_t length,
                                Annotation *items, int *count) {
    if (!text || !items || !count || *count < 0 || *count > MAX_ANNOTATIONS)
        return FALSE;
    JsonReader reader = {text, text + length, 0};
    if (!json_consume(&reader, '{')) return FALSE;
    BOOL found_annotations = FALSE;
    int parsed = 0;
    json_skip_space(&reader);
    if (reader.current < reader.end && *reader.current == '}') return FALSE;
    for (;;) {
        char key[128];
        if (!json_parse_key(&reader, key, sizeof(key)) ||
            !json_consume(&reader, ':')) return FALSE;
        if (!strcmp(key, "annotations")) {
            if (found_annotations || !json_parse_annotations(
                    &reader, items, *count, &parsed)) return FALSE;
            found_annotations = TRUE;
        } else if (!json_skip_value(&reader)) return FALSE;
        json_skip_space(&reader);
        if (reader.current < reader.end && *reader.current == '}') {
            ++reader.current;
            break;
        }
        if (!json_consume(&reader, ',')) return FALSE;
    }
    json_skip_space(&reader);
    if (!found_annotations || reader.current != reader.end) return FALSE;
    *count = parsed;
    return TRUE;
}

static BOOL reserve(TextBuffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->length - 1) return FALSE;
    size_t needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity) return TRUE;
    size_t capacity = buffer->capacity ? buffer->capacity : 512;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
        capacity *= 2;
    }
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

#if defined(__clang__) || defined(__GNUC__)
static BOOL append_format(TextBuffer *buffer, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
#endif

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
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        name, -1, NULL, 0, NULL, NULL);
    char *utf8 = length ? (char *)malloc((size_t)length) : NULL;
    if (!utf8) return FALSE;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                             name, -1, utf8, length, NULL, NULL)) {
        free(utf8);
        return FALSE;
    }
    BOOL ok = TRUE;
    for (const unsigned char *p = (unsigned char *)utf8; ok && *p; ++p) {
        if (*p == '"' || *p == '\\') { char escaped[2] = {'\\', (char)*p}; ok = append_bytes(buffer, escaped, 2); }
        else if (*p == '\b') ok = append_text(buffer, "\\b");
        else if (*p == '\f') ok = append_text(buffer, "\\f");
        else if (*p == '\n') ok = append_text(buffer, "\\n");
        else if (*p == '\r') ok = append_text(buffer, "\\r");
        else if (*p == '\t') ok = append_text(buffer, "\\t");
        else if (*p < 0x20) ok = append_format(buffer, "\\u%04x", *p);
        else ok = append_bytes(buffer, (char *)p, 1);
    }
    free(utf8);
    return ok;
}

char *golden_document_serialize_utf8(const Annotation *items, int count, size_t *length) {
    if (length) *length = 0;
    if (count < 0 || count > MAX_ANNOTATIONS || (count && !items)) return NULL;
    for (int i = 0; i < count; ++i) {
        const Annotation *annotation = &items[i];
        LONGLONG width = (LONGLONG)annotation->boundary.right -
                         annotation->boundary.left;
        LONGLONG height = (LONGLONG)annotation->boundary.bottom -
                          annotation->boundary.top;
        if (!annotation->name[0] ||
            !wmemchr(annotation->name, 0, _countof(annotation->name)) ||
            width <= 0 || width > LONG_MAX ||
            height <= 0 || height > LONG_MAX ||
            golden_name_exists(items, count, annotation->name, i) ||
            (annotation->has_click &&
             (!isfinite(annotation->click_x) || !isfinite(annotation->click_y) ||
              annotation->click_x < 0.0 || annotation->click_x > 1.0 ||
              annotation->click_y < 0.0 || annotation->click_y > 1.0))) return NULL;
    }
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

#include "clipboard_image.h"

#include <shellapi.h>
#include <shlobj.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static BOOL ends_with_png(const wchar_t *path) {
    size_t length = path ? wcslen(path) : 0;
    return length >= 4 && !_wcsicmp(path + length - 4, L".png");
}

static BOOL read_png_path(wchar_t *path, size_t capacity);

static HGLOBAL make_dib(const BYTE *pixels, UINT width, UINT height,
                        UINT stride) {
    if (!pixels || !width || !height || width > UINT32_MAX / 4 ||
        width > INT_MAX || height > INT_MAX || stride < width * 4) return NULL;
    size_t row_bytes = (size_t)width * 4;
    if (height > (SIZE_MAX - sizeof(BITMAPINFOHEADER)) / row_bytes)
        return NULL;
    size_t pixel_bytes = row_bytes * height;
    if (pixel_bytes > UINT32_MAX) return NULL;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE,
                                 sizeof(BITMAPINFOHEADER) + pixel_bytes);
    if (!memory) return NULL;
    BYTE *data = (BYTE *)GlobalLock(memory);
    if (!data) {
        GlobalFree(memory);
        return NULL;
    }
    BITMAPINFOHEADER *header = (BITMAPINFOHEADER *)data;
    *header = (BITMAPINFOHEADER){0};
    header->biSize = sizeof(*header);
    header->biWidth = (LONG)width;
    header->biHeight = (LONG)height;
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = (DWORD)pixel_bytes;
    BYTE *destination = data + sizeof(*header);
    for (UINT y = 0; y < height; ++y) {
        const BYTE *source_row = pixels + (size_t)y * stride;
        BYTE *destination_row = destination + (size_t)(height - y - 1) * row_bytes;
        memcpy(destination_row, source_row, row_bytes);
    }
    GlobalUnlock(memory);
    return memory;
}

static HGLOBAL make_file_drop(const wchar_t *path) {
    if (!path || !path[0]) return NULL;
    size_t characters = wcslen(path) + 2;
    if (characters > (SIZE_MAX - sizeof(DROPFILES)) / sizeof(wchar_t))
        return NULL;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
        sizeof(DROPFILES) + characters * sizeof(wchar_t));
    if (!memory) return NULL;
    DROPFILES *drop = (DROPFILES *)GlobalLock(memory);
    if (!drop) {
        GlobalFree(memory);
        return NULL;
    }
    drop->pFiles = sizeof(*drop);
    drop->fWide = TRUE;
    memcpy((BYTE *)drop + sizeof(*drop), path,
           (wcslen(path) + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    return memory;
}

BOOL golden_clipboard_copy_image(HWND owner, const BYTE *pixels,
                                 UINT width, UINT height, UINT stride,
                                 const wchar_t *png_path) {
    HGLOBAL dib = make_dib(pixels, width, height, stride);
    HGLOBAL drop = make_file_drop(png_path);
    if (!dib || !OpenClipboard(owner)) {
        if (dib) GlobalFree(dib);
        if (drop) GlobalFree(drop);
        return FALSE;
    }
    BOOL copied = FALSE;
    if (EmptyClipboard()) {
        if (SetClipboardData(CF_DIB, dib)) {
            dib = NULL;
            copied = TRUE;
        }
        if (drop && SetClipboardData(CF_HDROP, drop)) drop = NULL;
    }
    CloseClipboard();
    if (dib) GlobalFree(dib);
    if (drop) GlobalFree(drop);
    return copied;
}

BOOL golden_clipboard_has_image(void) {
    /* Menu state is refreshed often, so inspect each clipboard value once. */
    static DWORD cached_sequence;
    static BOOL cache_initialized;
    static BOOL cached_result;
    DWORD sequence = GetClipboardSequenceNumber();
    if (cache_initialized && sequence && sequence == cached_sequence)
        return cached_result;

    BOOL available = IsClipboardFormatAvailable(CF_DIBV5) ||
                     IsClipboardFormatAvailable(CF_DIB) ||
                     IsClipboardFormatAvailable(CF_BITMAP);
    if (!available && IsClipboardFormatAvailable(CF_HDROP)) {
        wchar_t path[MAX_PATH * 4];
        if (!OpenClipboard(NULL)) return FALSE;
        available = read_png_path(path, _countof(path));
        CloseClipboard();
    }

    DWORD confirmed_sequence = GetClipboardSequenceNumber();
    if (sequence && sequence == confirmed_sequence) {
        cached_sequence = sequence;
        cached_result = available;
        cache_initialized = TRUE;
    }
    return available;
}

static BOOL read_png_path(wchar_t *path, size_t capacity) {
    HDROP drop = (HDROP)GetClipboardData(CF_HDROP);
    if (!drop || !path || !capacity || capacity > UINT32_MAX) return FALSE;
    UINT count = DragQueryFileW(drop, UINT32_MAX, NULL, 0);
    for (UINT i = 0; i < count; ++i) {
        UINT length = DragQueryFileW(drop, i, NULL, 0);
        if ((size_t)length >= capacity ||
            !DragQueryFileW(drop, i, path, (UINT)capacity)) continue;
        DWORD attributes = GetFileAttributesW(path);
        if (ends_with_png(path) && attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return TRUE;
    }
    path[0] = 0;
    return FALSE;
}

static BYTE scale_mask_component(DWORD value, DWORD mask) {
    if (!mask) return 255;
    unsigned shift = 0;
    while (!(mask & 1u)) { mask >>= 1; ++shift; }
    DWORD maximum = mask;
    DWORD component = (value >> shift) & maximum;
    return maximum ? (BYTE)(((uint64_t)component * 255u + maximum / 2u) /
                            maximum) : 0;
}

static BOOL read_dib(UINT format, GoldenImage *image) {
    HGLOBAL memory = GetClipboardData(format);
    if (!memory || !image) return FALSE;
    size_t total = GlobalSize(memory);
    const BYTE *data = (const BYTE *)GlobalLock(memory);
    if (!data || total < sizeof(BITMAPINFOHEADER)) {
        if (data) GlobalUnlock(memory);
        return FALSE;
    }
    const BITMAPINFOHEADER *header = (const BITMAPINFOHEADER *)data;
    BOOL bitfields = header->biCompression == BI_BITFIELDS;
    if (header->biSize < sizeof(*header) || header->biSize > total ||
        header->biWidth <= 0 || !header->biHeight ||
        header->biHeight == LONG_MIN || header->biPlanes != 1 ||
        (header->biBitCount != 24 && header->biBitCount != 32) ||
        (header->biCompression != BI_RGB && !bitfields) ||
        (bitfields && (header->biBitCount != 32 ||
                       (header->biSize != sizeof(*header) &&
                        header->biSize < 52))) || header->biClrUsed) {
        GlobalUnlock(memory);
        return FALSE;
    }
    UINT width = (UINT)header->biWidth;
    UINT height = (UINT)(header->biHeight < 0 ? -header->biHeight : header->biHeight);
    uint64_t source_stride64 = ((uint64_t)width * header->biBitCount + 31u) / 32u * 4u;
    if (!height || width > UINT32_MAX / 4 || source_stride64 > SIZE_MAX ||
        height > SIZE_MAX / (size_t)source_stride64) {
        GlobalUnlock(memory);
        return FALSE;
    }
    size_t bits_offset = header->biSize;
    DWORD red_mask = 0x00ff0000u, green_mask = 0x0000ff00u;
    DWORD blue_mask = 0x000000ffu, alpha_mask = 0;
    if (bitfields) {
        size_t masks_offset = sizeof(BITMAPINFOHEADER);
        if (header->biSize == sizeof(BITMAPINFOHEADER)) {
            if (bits_offset > SIZE_MAX - 3 * sizeof(DWORD)) {
                GlobalUnlock(memory);
                return FALSE;
            }
            bits_offset += 3 * sizeof(DWORD);
        }
        if (masks_offset + 3 * sizeof(DWORD) > total || bits_offset > total) {
            GlobalUnlock(memory);
            return FALSE;
        }
        const DWORD *masks = (const DWORD *)(data + masks_offset);
        red_mask = masks[0]; green_mask = masks[1]; blue_mask = masks[2];
        if (header->biSize >= 56 && masks_offset + 4 * sizeof(DWORD) <= total)
            alpha_mask = masks[3];
    }
    size_t source_stride = (size_t)source_stride64;
    size_t source_bytes = source_stride * height;
    size_t destination_stride = (size_t)width * 4;
    if (bits_offset > total || source_bytes > total - bits_offset ||
        height > SIZE_MAX / destination_stride) {
        GlobalUnlock(memory);
        return FALSE;
    }
    BYTE *pixels = (BYTE *)malloc(destination_stride * height);
    if (!pixels) {
        GlobalUnlock(memory);
        return FALSE;
    }
    const BYTE *bits = data + bits_offset;
    for (UINT y = 0; y < height; ++y) {
        UINT source_y = header->biHeight < 0 ? y : height - y - 1;
        const BYTE *source = bits + (size_t)source_y * source_stride;
        BYTE *destination = pixels + (size_t)y * destination_stride;
        for (UINT x = 0; x < width; ++x) {
            if (header->biBitCount == 24) {
                destination[0] = source[0];
                destination[1] = source[1];
                destination[2] = source[2];
                destination[3] = 255;
                source += 3;
            } else if (!bitfields) {
                destination[0] = source[0];
                destination[1] = source[1];
                destination[2] = source[2];
                destination[3] = 255;
                source += 4;
            } else {
                DWORD value;
                memcpy(&value, source, sizeof(value));
                destination[0] = scale_mask_component(value, blue_mask);
                destination[1] = scale_mask_component(value, green_mask);
                destination[2] = scale_mask_component(value, red_mask);
                destination[3] = alpha_mask ?
                    scale_mask_component(value, alpha_mask) : 255;
                source += 4;
            }
            destination += 4;
        }
    }
    GlobalUnlock(memory);
    golden_image_free(image);
    image->pixels = pixels;
    image->width = width;
    image->height = height;
    image->stride = (UINT)destination_stride;
    return TRUE;
}

static BOOL read_bitmap(GoldenImage *image) {
    HBITMAP bitmap = (HBITMAP)GetClipboardData(CF_BITMAP);
    BITMAP details;
    if (!bitmap || !image ||
        GetObjectW(bitmap, (int)sizeof(details), &details) !=
            (int)sizeof(details) ||
        details.bmWidth <= 0 || details.bmHeight <= 0 ||
        (UINT)details.bmWidth > UINT32_MAX / 4) return FALSE;
    UINT width = (UINT)details.bmWidth;
    UINT height = (UINT)details.bmHeight;
    size_t stride = (size_t)width * 4;
    if (height > SIZE_MAX / stride) return FALSE;
    BYTE *pixels = (BYTE *)malloc(stride * height);
    if (!pixels) return FALSE;
    BITMAPINFO info = {0};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = (LONG)width;
    info.bmiHeader.biHeight = -(LONG)height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(NULL);
    int rows = dc ? GetDIBits(dc, bitmap, 0, height, pixels, &info, DIB_RGB_COLORS) : 0;
    if (dc) ReleaseDC(NULL, dc);
    if (rows != (int)height) {
        free(pixels);
        return FALSE;
    }
    golden_bgra_force_opaque(pixels, width, height, (UINT)stride);
    golden_image_free(image);
    image->pixels = pixels;
    image->width = width;
    image->height = height;
    image->stride = (UINT)stride;
    return TRUE;
}

GoldenClipboardContent golden_clipboard_read_image(
    HWND owner, GoldenImage *image, wchar_t *png_path, size_t path_capacity) {
    if (image) golden_image_free(image);
    if (png_path && path_capacity) png_path[0] = 0;
    if (!OpenClipboard(owner)) return GOLDEN_CLIPBOARD_NONE;
    GoldenClipboardContent content = GOLDEN_CLIPBOARD_NONE;
    if (read_png_path(png_path, path_capacity))
        content = GOLDEN_CLIPBOARD_PNG_PATH;
    else if (read_dib(CF_DIBV5, image) || read_dib(CF_DIB, image) ||
             read_bitmap(image))
        content = GOLDEN_CLIPBOARD_PIXELS;
    CloseClipboard();
    return content;
}

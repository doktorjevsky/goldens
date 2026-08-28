#ifndef GOLDENS_CLIPBOARD_IMAGE_H
#define GOLDENS_CLIPBOARD_IMAGE_H

#include <windows.h>
#include <stddef.h>

#include "image_io.h"

typedef enum {
    GOLDEN_CLIPBOARD_NONE,
    GOLDEN_CLIPBOARD_PNG_PATH,
    GOLDEN_CLIPBOARD_PIXELS
} GoldenClipboardContent;

BOOL golden_clipboard_copy_image(HWND owner, const BYTE *pixels,
                                 UINT width, UINT height, UINT stride,
                                 const wchar_t *png_path);
BOOL golden_clipboard_has_image(void);
GoldenClipboardContent golden_clipboard_read_image(
    HWND owner, GoldenImage *image, wchar_t *png_path, size_t path_capacity);

#endif

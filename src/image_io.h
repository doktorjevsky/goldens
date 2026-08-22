#ifndef GOLDENS_IMAGE_IO_H
#define GOLDENS_IMAGE_IO_H

#include <windows.h>
#include <wincodec.h>

typedef struct {
    BYTE *pixels;
    UINT width;
    UINT height;
    UINT stride;
} GoldenImage;

BOOL golden_png_load(IWICImagingFactory *factory, const wchar_t *path, GoldenImage *image);
BOOL golden_png_save(IWICImagingFactory *factory, const wchar_t *path,
                     const BYTE *pixels, UINT width, UINT height, UINT stride);
void golden_image_free(GoldenImage *image);

#endif

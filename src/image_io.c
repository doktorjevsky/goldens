#include "image_io.h"
#include "atomic_file.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

void golden_image_free(GoldenImage *image) {
    if (!image) return;
    free(image->pixels);
    *image = (GoldenImage){0};
}

void golden_bgra_force_opaque(BYTE *pixels, UINT width, UINT height, UINT stride) {
    if (!pixels || !width || !height || width > UINT32_MAX / 4 || stride < width * 4) return;
    for (UINT y = 0; y < height; ++y) {
        BYTE *row = pixels + (size_t)y * stride;
        for (UINT x = 0; x < width; ++x) row[(size_t)x * 4 + 3] = 255;
    }
}

BOOL golden_png_load(IWICImagingFactory *factory, const wchar_t *path, GoldenImage *image) {
    if (!factory || !path || !image) return FALSE;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    GoldenImage loaded = {0};
    HRESULT hr = IWICImagingFactory_CreateDecoderFromFilename(factory, path, NULL,
        GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    GUID container = {0};
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetContainerFormat(decoder, &container);
    if (SUCCEEDED(hr) && !IsEqualGUID(&container, &GUID_ContainerFormatPng))
        hr = WINCODEC_ERR_BADIMAGE;
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameDecode_GetSize(frame, &loaded.width, &loaded.height);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(converter,
        (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        if (!loaded.width || !loaded.height || loaded.width > INT_MAX ||
            loaded.height > INT_MAX || loaded.width > UINT32_MAX / 4) {
            hr = E_FAIL;
        } else {
            size_t stride = (size_t)loaded.width * 4;
            if (loaded.height > UINT32_MAX / stride) {
                hr = E_FAIL;
            } else {
                size_t bytes = stride * loaded.height;
                loaded.stride = (UINT)stride;
                loaded.pixels = (BYTE *)malloc(bytes);
                if (!loaded.pixels) hr = E_OUTOFMEMORY;
                else hr = IWICFormatConverter_CopyPixels(
                    converter, NULL, loaded.stride, (UINT)bytes, loaded.pixels);
            }
        }
    }
    if (converter) IWICFormatConverter_Release(converter);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (FAILED(hr)) { golden_image_free(&loaded); return FALSE; }
    golden_image_free(image);
    *image = loaded;
    return TRUE;
}

typedef struct {
    IWICImagingFactory *factory;
    const BYTE *pixels;
    UINT width;
    UINT height;
    UINT stride;
} PngWriterContext;

static BOOL png_write(const wchar_t *path, void *opaque) {
    PngWriterContext *context = (PngWriterContext *)opaque;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    HRESULT hr = IWICImagingFactory_CreateStream(context->factory, &stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(context->factory,
        &GUID_ContainerFormatPng, NULL, &encoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream,
        WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &properties);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(frame, properties);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(
        frame, context->width, context->height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (SUCCEEDED(hr) && !IsEqualGUID(&format, &GUID_WICPixelFormat32bppBGRA)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(
        frame, context->height, context->stride,
        context->stride * context->height, (BYTE *)context->pixels);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(encoder);
    if (properties) IPropertyBag2_Release(properties);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    return SUCCEEDED(hr);
}

BOOL golden_png_save(IWICImagingFactory *factory, const wchar_t *path,
                     const BYTE *pixels, UINT width, UINT height, UINT stride) {
    if (!factory || !path || !pixels || !width || !height ||
        width > UINT32_MAX / 4 || stride < width * 4 ||
        stride > UINT32_MAX / height) return FALSE;
    PngWriterContext context = {factory, pixels, width, height, stride};
    return golden_atomic_replace_file(path, png_write, &context);
}

#include "image_io.h"

#include <stdint.h>
#include <stdlib.h>

void golden_image_free(GoldenImage *image) {
    if (!image) return;
    free(image->pixels);
    *image = (GoldenImage){0};
}

BOOL golden_png_load(IWICImagingFactory *factory, const wchar_t *path, GoldenImage *image) {
    if (!factory || !path || !image) return FALSE;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    GoldenImage loaded = {0};
    HRESULT hr = IWICImagingFactory_CreateDecoderFromFilename(factory, path, NULL,
        GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameDecode_GetSize(frame, &loaded.width, &loaded.height);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(factory, &converter);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(converter,
        (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) {
        size_t stride = (size_t)loaded.width * 4;
        size_t bytes = stride * loaded.height;
        if (!loaded.width || !loaded.height || bytes > UINT32_MAX) hr = E_FAIL;
        else {
            loaded.stride = (UINT)stride;
            loaded.pixels = (BYTE *)malloc(bytes);
            if (!loaded.pixels) hr = E_OUTOFMEMORY;
            else hr = IWICFormatConverter_CopyPixels(converter, NULL, loaded.stride,
                                                      (UINT)bytes, loaded.pixels);
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

BOOL golden_png_save(IWICImagingFactory *factory, const wchar_t *path,
                     const BYTE *pixels, UINT width, UINT height, UINT stride) {
    if (!factory || !path || !pixels || !width || !height || stride < width * 4) return FALSE;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *encoder = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *properties = NULL;
    HRESULT hr = IWICImagingFactory_CreateStream(factory, &stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(factory,
        &GUID_ContainerFormatPng, NULL, &encoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(encoder, (IStream *)stream,
        WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(encoder, &frame, &properties);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(frame, properties);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(frame, width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &format);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(frame, height, stride,
                                                               stride * height, (BYTE *)pixels);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(encoder);
    if (properties) IPropertyBag2_Release(properties);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (encoder) IWICBitmapEncoder_Release(encoder);
    if (stream) IWICStream_Release(stream);
    return SUCCEEDED(hr);
}

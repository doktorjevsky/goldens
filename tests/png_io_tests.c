#include "../src/image_io.h"
#include "third_party/lodepng/lodepng.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PNGSUITE_DIRECTORY L"tests\\fixtures\\pngsuite"
#define PNGSUITE_EXPECTED_FILES 161

static uint32_t read_be32(const BYTE *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static uint32_t png_crc32(const BYTE *bytes, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc ^ UINT32_MAX;
}

static BOOL read_file_bytes(const wchar_t *path, BYTE **bytes, size_t *size) {
    *bytes = NULL;
    *size = 0;
    FILE *file = _wfopen(path, L"rb");
    if (!file) return FALSE;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return FALSE; }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return FALSE; }
    *size = (size_t)length;
    *bytes = (BYTE *)malloc(*size ? *size : 1);
    BOOL ok = *bytes && fread(*bytes, 1, *size, file) == *size;
    fclose(file);
    if (!ok) {
        free(*bytes);
        *bytes = NULL;
        *size = 0;
    }
    return ok;
}

static BOOL write_file_bytes(const wchar_t *path, const BYTE *bytes, size_t size) {
    FILE *file = _wfopen(path, L"wb");
    if (!file) return FALSE;
    BOOL ok = fwrite(bytes, 1, size, file) == size;
    if (fclose(file)) ok = FALSE;
    return ok;
}

static BOOL make_temp_path(wchar_t *path, size_t capacity) {
    wchar_t folder[MAX_PATH];
    if (capacity < MAX_PATH || !GetTempPathW(MAX_PATH, folder) ||
        !GetTempFileNameW(folder, L"gld", 0, path))
        return FALSE;
    DeleteFileW(path);
    return TRUE;
}

static BOOL validate_encoded_png(const BYTE *png, size_t size, UINT width, UINT height) {
    static const BYTE signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size < sizeof(signature) || memcmp(png, signature, sizeof(signature))) {
        fwprintf(stderr, L"encoded file has an invalid PNG signature\n");
        return FALSE;
    }

    size_t offset = sizeof(signature);
    BOOL seen_ihdr = FALSE, seen_plte = FALSE, seen_idat = FALSE, ended_idat = FALSE;
    while (offset < size) {
        if (size - offset < 12) {
            fwprintf(stderr, L"encoded file has a truncated chunk\n");
            return FALSE;
        }
        uint32_t length = read_be32(png + offset);
        if ((size_t)length > size - offset - 12) {
            fwprintf(stderr, L"encoded file has an out-of-bounds chunk length\n");
            return FALSE;
        }
        const BYTE *type = png + offset + 4;
        const BYTE *data = type + 4;
        for (int i = 0; i < 4; ++i) {
            if (!((type[i] >= 'A' && type[i] <= 'Z') ||
                  (type[i] >= 'a' && type[i] <= 'z'))) {
                fwprintf(stderr, L"encoded file has an invalid chunk type\n");
                return FALSE;
            }
        }
        if (type[2] & 0x20) {
            fwprintf(stderr, L"encoded file has an invalid reserved chunk bit\n");
            return FALSE;
        }
        uint32_t stored_crc = read_be32(data + length);
        if (png_crc32(type, (size_t)length + 4) != stored_crc) {
            fwprintf(stderr, L"encoded file has a bad chunk CRC\n");
            return FALSE;
        }

        BOOL ihdr = !memcmp(type, "IHDR", 4);
        BOOL plte = !memcmp(type, "PLTE", 4);
        BOOL idat = !memcmp(type, "IDAT", 4);
        BOOL iend = !memcmp(type, "IEND", 4);
        if (!seen_ihdr) {
            if (!ihdr || length != 13 || read_be32(data) != width ||
                read_be32(data + 4) != height || data[8] != 8 || data[9] != 6 ||
                data[10] || data[11] || data[12] > 1) {
                fwprintf(stderr, L"encoded file has an invalid IHDR\n");
                return FALSE;
            }
            seen_ihdr = TRUE;
        } else if (ihdr || (plte && (seen_plte || seen_idat)) || (idat && ended_idat)) {
            fwprintf(stderr, L"encoded file has invalid critical-chunk ordering\n");
            return FALSE;
        } else if (plte) {
            seen_plte = TRUE;
        } else if (idat) {
            seen_idat = TRUE;
        } else if (seen_idat && !iend) {
            ended_idat = TRUE;
        }

        offset += (size_t)length + 12;
        if (iend) {
            if (length || !seen_idat || offset != size) {
                fwprintf(stderr, L"encoded file has an invalid IEND\n");
                return FALSE;
            }
            return TRUE;
        }
        if (!(type[0] & 0x20) && !ihdr && !plte && !idat) {
            fwprintf(stderr, L"encoded file has an unknown critical chunk\n");
            return FALSE;
        }
    }
    fwprintf(stderr, L"encoded file has no IEND\n");
    return FALSE;
}

static BOOL compare_bgra_to_rgba(const BYTE *bgra, UINT bgra_stride,
                                 const BYTE *rgba, UINT width, UINT height,
                                 UINT tolerance, const wchar_t *label) {
    for (UINT y = 0; y < height; ++y) {
        const BYTE *bgra_row = bgra + (size_t)y * bgra_stride;
        const BYTE *rgba_row = rgba + (size_t)y * width * 4;
        for (UINT x = 0; x < width; ++x) {
            const BYTE *b = bgra_row + (size_t)x * 4;
            const BYTE *r = rgba_row + (size_t)x * 4;
            /* RGB is not observable when both straight-alpha pixels are fully
               transparent. WIC canonicalizes that hidden color to black while
               other conforming decoders may preserve the encoded samples. */
            BOOL hidden_rgb = b[3] == 0 && r[3] == 0;
            BOOL color_mismatch = !hidden_rgb &&
                (abs((int)b[0] - r[2]) > (int)tolerance ||
                 abs((int)b[1] - r[1]) > (int)tolerance ||
                 abs((int)b[2] - r[0]) > (int)tolerance);
            if (color_mismatch ||
                abs((int)b[3] - r[3]) > (int)tolerance) {
                fwprintf(stderr, L"pixel mismatch in %ls at %u,%u\n", label, x, y);
                return FALSE;
            }
        }
    }
    return TRUE;
}

static BOOL decode_reference(const wchar_t *path, BYTE **rgba, UINT *width, UINT *height,
                             UINT *bit_depth) {
    BYTE *png = NULL;
    size_t png_size = 0;
    if (!read_file_bytes(path, &png, &png_size)) return FALSE;
    *bit_depth = png_size > 24 ? png[24] : 0;
    unsigned reference_width = 0, reference_height = 0;
    unsigned error = lodepng_decode32(rgba, &reference_width, &reference_height, png, png_size);
    free(png);
    if (error) {
        fwprintf(stderr, L"LodePNG rejected %ls with error %u\n", path, error);
        return FALSE;
    }
    *width = reference_width;
    *height = reference_height;
    return TRUE;
}

static BOOL test_pngsuite(IWICImagingFactory *factory) {
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    _snwprintf(pattern, _countof(pattern), L"%ls\\*.png", PNGSUITE_DIRECTORY);
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"PngSuite fixtures are missing; run tests from the repository root\n");
        return FALSE;
    }

    unsigned count = 0;
    BOOL ok = TRUE;
    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        _snwprintf(path, _countof(path), L"%ls\\%ls", PNGSUITE_DIRECTORY, entry.cFileName);
        BYTE *reference = NULL;
        UINT reference_width = 0, reference_height = 0, reference_bit_depth = 0;
        GoldenImage loaded = {0};
        if (!decode_reference(path, &reference, &reference_width, &reference_height,
                              &reference_bit_depth) ||
            !golden_png_load(factory, path, &loaded)) {
            fwprintf(stderr, L"could not decode valid PngSuite image %ls\n", entry.cFileName);
            ok = FALSE;
        } else if (loaded.width != reference_width || loaded.height != reference_height ||
                   loaded.stride != loaded.width * 4 ||
                   !compare_bgra_to_rgba(loaded.pixels, loaded.stride, reference,
                                         loaded.width, loaded.height,
                                         /* PNG permits rounding or low-byte truncation here. */
                                         reference_bit_depth == 16 ? 1 : 0,
                                         entry.cFileName)) {
            fwprintf(stderr, L"PngSuite mismatch for %ls\n", entry.cFileName);
            ok = FALSE;
        }
        free(reference);
        golden_image_free(&loaded);
        ++count;
    } while (ok && FindNextFileW(find, &entry));
    FindClose(find);
    if (ok && count != PNGSUITE_EXPECTED_FILES) {
        fwprintf(stderr, L"expected %u PngSuite images, found %u\n",
                 PNGSUITE_EXPECTED_FILES, count);
        ok = FALSE;
    }
    return ok;
}

static void fill_bgra(BYTE *pixels, UINT width, UINT height, UINT stride, UINT seed) {
    memset(pixels, 0xa5, (size_t)stride * height);
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
        BYTE *pixel = pixels + (size_t)y * stride + (size_t)x * 4;
        pixel[0] = (BYTE)(x * 31 + y * 7 + seed);
        pixel[1] = (BYTE)(y * 61 + x * 3 + seed * 5);
        pixel[2] = (BYTE)(x * 19 + y * 23 + seed * 11);
        pixel[3] = (BYTE)(x * 47 + y * 53 + seed * 17);
    }
}

static BOOL test_encoder_case(IWICImagingFactory *factory, UINT width, UINT height,
                              UINT stride, UINT seed) {
    size_t bytes = (size_t)stride * height;
    BYTE *source = (BYTE *)malloc(bytes);
    if (!source) return FALSE;
    fill_bgra(source, width, height, stride, seed);

    wchar_t path[MAX_PATH] = L"";
    BYTE *png = NULL, *reference = NULL;
    size_t png_size = 0;
    unsigned reference_width = 0, reference_height = 0;
    BOOL ok = make_temp_path(path, _countof(path)) &&
              golden_png_save(factory, path, source, width, height, stride) &&
              read_file_bytes(path, &png, &png_size) &&
              validate_encoded_png(png, png_size, width, height);
    unsigned error = ok ? lodepng_decode32(&reference, &reference_width, &reference_height,
                                           png, png_size) : 1;
    if (ok && error) {
        fwprintf(stderr, L"LodePNG rejected WIC output with error %u\n", error);
        ok = FALSE;
    }
    if (ok && (reference_width != width || reference_height != height ||
               !compare_bgra_to_rgba(source, stride, reference, width, height,
                                     0, L"WIC encoder output")))
        ok = FALSE;

    if (path[0]) DeleteFileW(path);
    free(reference);
    free(png);
    free(source);
    return ok;
}

static BOOL test_encoder(IWICImagingFactory *factory) {
    return test_encoder_case(factory, 1, 1, 4, 0) &&
           test_encoder_case(factory, 5, 3, 28, 1) &&
           test_encoder_case(factory, 31, 7, 31 * 4, 2);
}

static BOOL test_invalid_inputs(IWICImagingFactory *factory) {
    wchar_t path[MAX_PATH];
    BYTE pixel[4] = {1, 2, 3, 4};
    if (!make_temp_path(path, _countof(path))) return FALSE;
    if (golden_png_save(factory, path, pixel, 0, 1, 4) ||
        golden_png_save(factory, path, pixel, 1, 0, 4) ||
        golden_png_save(factory, path, pixel, 1, 1, 3) ||
        golden_png_save(factory, path, pixel, UINT32_MAX, 1, UINT32_MAX) ||
        golden_png_save(factory, path, pixel, 1, UINT32_MAX, 4)) {
        fwprintf(stderr, L"PNG encoder accepted invalid geometry\n");
        DeleteFileW(path);
        return FALSE;
    }

    static const BYTE malformed[] = {137, 80, 78, 71, 13, 10, 26, 10,
                                      0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    GoldenImage loaded = {0};
    BOOL ok = write_file_bytes(path, malformed, sizeof(malformed)) &&
              !golden_png_load(factory, path, &loaded) && !loaded.pixels;
    static const BYTE bitmap[] = {
        'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
        40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0,
        0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 3, 2, 1, 0
    };
    if (ok) ok = write_file_bytes(path, bitmap, sizeof(bitmap)) &&
                 !golden_png_load(factory, path, &loaded) && !loaded.pixels;
    golden_image_free(&loaded);
    DeleteFileW(path);
    if (!ok) fwprintf(stderr, L"PNG decoder accepted malformed or non-PNG input\n");
    return ok;
}

static BOOL test_opaque_normalization(void) {
    BYTE pixels[2 * 12];
    memset(pixels, 0x5a, sizeof(pixels));
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x)
        pixels[y * 12 + x * 4 + 3] = (BYTE)(x + y);
    golden_bgra_force_opaque(pixels, 2, 2, 12);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x)
            if (pixels[y * 12 + x * 4 + 3] != 255) return FALSE;
        for (int x = 8; x < 12; ++x)
            if (pixels[y * 12 + x] != 0x5a) return FALSE;
    }
    return TRUE;
}

static int image_matches(IWICImagingFactory *factory, const wchar_t *path,
                         const BYTE *expected, UINT width, UINT height) {
    GoldenImage loaded = {0};
    int matches = golden_png_load(factory, path, &loaded) &&
        loaded.width == width && loaded.height == height &&
        loaded.stride == width * 4 &&
        !memcmp(expected, loaded.pixels, (size_t)loaded.stride * height);
    golden_image_free(&loaded);
    return matches;
}

static BOOL test_atomic_replacement(IWICImagingFactory *factory) {
    BYTE source[5 * 3 * 4];
    fill_bgra(source, 5, 3, 20, 3);
    wchar_t path[MAX_PATH] = L"";
    if (!make_temp_path(path, _countof(path))) return FALSE;

    BOOL ok = golden_png_save(factory, path, source, 5, 3, 20) &&
              image_matches(factory, path, source, 5, 3);
    if (ok) ok = !golden_png_save(factory, path, NULL, 5, 3, 20) &&
                 image_matches(factory, path, source, 5, 3);
    if (ok) ok = !golden_png_save(factory, path, source, UINT32_MAX, 1,
                                  UINT32_MAX) &&
                 image_matches(factory, path, source, 5, 3);

    BYTE replacement[sizeof(source)];
    memcpy(replacement, source, sizeof(source));
    for (size_t i = 0; i < sizeof(replacement); i += 4)
        replacement[i] ^= 0xff;
    if (ok) ok = golden_png_save(factory, path, replacement, 5, 3, 20) &&
                 image_matches(factory, path, replacement, 5, 3);

    wchar_t pattern[MAX_PATH * 2];
    _snwprintf(pattern, _countof(pattern), L"%ls.goldens-*.tmp", path);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        ok = FALSE;
        FindClose(search);
    }
    DeleteFileW(path);
    if (!ok) fwprintf(stderr, L"atomic PNG replacement test failed\n");
    return ok;
}

int wmain(void) {
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;
    IWICImagingFactory *factory = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr)) {
        CoUninitialize();
        return 2;
    }
    BOOL ok = test_encoder(factory) && test_pngsuite(factory) &&
              test_atomic_replacement(factory) && test_invalid_inputs(factory) &&
              test_opaque_normalization();
    IWICImagingFactory_Release(factory);
    CoUninitialize();
    if (!ok) return 1;
    puts("All Goldens PNG interoperability tests passed (161 PngSuite images).");
    return 0;
}

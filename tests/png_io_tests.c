#include "../src/image_io.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

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

int wmain(void) {
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;
    IWICImagingFactory *factory = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr)) { CoUninitialize(); return 2; }
    BYTE source[5 * 3 * 4];
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 5; ++x) {
        int offset = (y * 5 + x) * 4;
        source[offset + 0] = (BYTE)(x * 31 + y);
        source[offset + 1] = (BYTE)(y * 61 + x);
        source[offset + 2] = (BYTE)(x * 19 + y * 7);
        source[offset + 3] = (BYTE)(255 - x * 13 - y * 3);
    }
    wchar_t folder[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, folder);
    GetTempFileNameW(folder, L"gld", 0, path);
    DeleteFileW(path);
    wcscat(path, L".png");
    int result = 0;
    if (!golden_png_save(factory, path, source, 5, 3, 20)) result = 3;
    if (!result && !image_matches(factory, path, source, 5, 3)) result = 4;

    if (!result && golden_png_save(factory, path, NULL, 5, 3, 20)) result = 5;
    if (!result && !image_matches(factory, path, source, 5, 3)) result = 6;
    if (!result && golden_png_save(factory, path, source, UINT32_MAX, 1,
                                   UINT32_MAX)) result = 7;
    if (!result && !image_matches(factory, path, source, 5, 3)) result = 8;

    BYTE replacement[sizeof(source)];
    memcpy(replacement, source, sizeof(source));
    for (size_t i = 0; i < sizeof(replacement); i += 4)
        replacement[i] ^= 0xff;
    if (!result && !golden_png_save(factory, path, replacement, 5, 3, 20)) result = 9;
    if (!result && !image_matches(factory, path, replacement, 5, 3)) result = 10;

    wchar_t pattern[MAX_PATH * 2];
    _snwprintf(pattern, _countof(pattern), L"%s.goldens-*.tmp", path);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (!result && search != INVALID_HANDLE_VALUE) result = 11;
    if (search != INVALID_HANDLE_VALUE) FindClose(search);
    DeleteFileW(path);
    IWICImagingFactory_Release(factory);
    CoUninitialize();
    if (result) return result;
    puts("All Goldens PNG fidelity tests passed.");
    return 0;
}

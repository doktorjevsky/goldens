#include "../src/image_io.h"

#include <stdio.h>
#include <string.h>

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
    GoldenImage loaded = {0};
    if (!result && !golden_png_load(factory, path, &loaded)) result = 4;
    if (!result && (loaded.width != 5 || loaded.height != 3 || loaded.stride != 20)) result = 5;
    if (!result && memcmp(source, loaded.pixels, sizeof(source))) result = 6;
    golden_image_free(&loaded);
    DeleteFileW(path);
    IWICImagingFactory_Release(factory);
    CoUninitialize();
    if (result) return result;
    puts("All Goldens PNG fidelity tests passed.");
    return 0;
}

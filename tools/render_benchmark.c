#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

#include "../src/editor_render.h"

enum {
    SOURCE_WIDTH = 1920,
    SOURCE_HEIGHT = 1080,
    VIEW_WIDTH = 1600,
    VIEW_HEIGHT = 900,
    FRAME_COUNT = 180
};

typedef struct {
    HDC screen;
    HDC target;
    HBITMAP target_bitmap;
    HGDIOBJ target_original;
    BYTE *pixels;
    LARGE_INTEGER frequency;
} Benchmark;

static RECT frame_destination(int frame) {
    int x = -700 + (frame % 120) * 4;
    int y = -380 + (frame % 90) * 3;
    return (RECT){x, y, x + 2880, y + 1620};
}

static double elapsed_ms(const Benchmark *benchmark,
                         LARGE_INTEGER start, LARGE_INTEGER end) {
    return (double)(end.QuadPart - start.QuadPart) * 1000.0 /
           (double)benchmark->frequency.QuadPart / FRAME_COUNT;
}

static void begin_frame(HDC dc) {
    RECT client = {0, 0, VIEW_WIDTH, VIEW_HEIGHT};
    FillRect(dc, &client, (HBRUSH)(COLOR_APPWORKSPACE + 1));
}

static double benchmark_legacy(Benchmark *benchmark) {
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    for (int frame = 0; frame < FRAME_COUNT; ++frame) {
        HDC buffer_dc = CreateCompatibleDC(benchmark->screen);
        HBITMAP buffer = CreateCompatibleBitmap(benchmark->screen,
                                                VIEW_WIDTH, VIEW_HEIGHT);
        HGDIOBJ previous = SelectObject(buffer_dc, buffer);
        begin_frame(buffer_dc);
        RECT destination = frame_destination(frame);
        golden_draw_bgra_image(buffer_dc, benchmark->pixels,
                               SOURCE_WIDTH, SOURCE_HEIGHT,
                               &destination, 1.5);
        BitBlt(benchmark->target, 0, 0, VIEW_WIDTH, VIEW_HEIGHT,
               buffer_dc, 0, 0, SRCCOPY);
        SelectObject(buffer_dc, previous);
        DeleteObject(buffer);
        DeleteDC(buffer_dc);
        GdiFlush();
    }
    QueryPerformanceCounter(&end);
    return elapsed_ms(benchmark, start, end);
}

static double benchmark_retained_buffer(Benchmark *benchmark) {
    GoldenBackBuffer buffer = {0};
    if (!golden_back_buffer_ensure(&buffer, benchmark->screen,
                                   VIEW_WIDTH, VIEW_HEIGHT)) return -1.0;
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    for (int frame = 0; frame < FRAME_COUNT; ++frame) {
        begin_frame(buffer.dc);
        RECT destination = frame_destination(frame);
        golden_draw_bgra_image(buffer.dc, benchmark->pixels,
                               SOURCE_WIDTH, SOURCE_HEIGHT,
                               &destination, 1.5);
        BitBlt(benchmark->target, 0, 0, VIEW_WIDTH, VIEW_HEIGHT,
               buffer.dc, 0, 0, SRCCOPY);
        GdiFlush();
    }
    QueryPerformanceCounter(&end);
    golden_back_buffer_release(&buffer);
    return elapsed_ms(benchmark, start, end);
}

static double benchmark_cached_image(Benchmark *benchmark) {
    GoldenBackBuffer buffer = {0};
    GoldenImageCache cache = {0};
    if (!golden_back_buffer_ensure(&buffer, benchmark->screen,
                                   VIEW_WIDTH, VIEW_HEIGHT)) return -1.0;
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    for (int frame = 0; frame < FRAME_COUNT; ++frame) {
        begin_frame(buffer.dc);
        RECT destination = frame_destination(frame);
        if (!golden_draw_cached_bgra_image(&cache, buffer.dc, benchmark->pixels,
                                           SOURCE_WIDTH, SOURCE_HEIGHT,
                                           &destination, 1.5, 1)) {
            golden_image_cache_release(&cache);
            golden_back_buffer_release(&buffer);
            return -1.0;
        }
        BitBlt(benchmark->target, 0, 0, VIEW_WIDTH, VIEW_HEIGHT,
               buffer.dc, 0, 0, SRCCOPY);
        GdiFlush();
    }
    QueryPerformanceCounter(&end);
    golden_image_cache_release(&cache);
    golden_back_buffer_release(&buffer);
    return elapsed_ms(benchmark, start, end);
}

int main(void) {
    Benchmark benchmark = {0};
    QueryPerformanceFrequency(&benchmark.frequency);
    benchmark.screen = GetDC(NULL);
    benchmark.target = CreateCompatibleDC(benchmark.screen);
    benchmark.target_bitmap = CreateCompatibleBitmap(benchmark.screen,
                                                      VIEW_WIDTH, VIEW_HEIGHT);
    benchmark.pixels = (BYTE *)malloc((size_t)SOURCE_WIDTH * SOURCE_HEIGHT * 4);
    if (!benchmark.screen || !benchmark.target || !benchmark.target_bitmap ||
        !benchmark.pixels) return 1;
    benchmark.target_original = SelectObject(benchmark.target,
                                             benchmark.target_bitmap);
    for (size_t i = 0; i < (size_t)SOURCE_WIDTH * SOURCE_HEIGHT; ++i) {
        benchmark.pixels[i * 4 + 0] = (BYTE)i;
        benchmark.pixels[i * 4 + 1] = (BYTE)(i >> 3);
        benchmark.pixels[i * 4 + 2] = (BYTE)(i >> 7);
        benchmark.pixels[i * 4 + 3] = 255;
    }

    double legacy = benchmark_legacy(&benchmark);
    double retained = benchmark_retained_buffer(&benchmark);
    double cached = benchmark_cached_image(&benchmark);
    printf("legacy:  %.3f ms/frame\n", legacy);
    printf("retained: %.3f ms/frame\n", retained);
    printf("cached:    %.3f ms/frame\n", cached);
    if (retained > 0.0 && cached > 0.0) {
        printf("cached speedup: %.2fx\n", legacy / cached);
        printf("cached capacity: %.0f fps\n", 1000.0 / cached);
    }

    free(benchmark.pixels);
    SelectObject(benchmark.target, benchmark.target_original);
    DeleteObject(benchmark.target_bitmap);
    DeleteDC(benchmark.target);
    ReleaseDC(NULL, benchmark.screen);
    return legacy > 0.0 && retained > 0.0 && cached > 0.0 ? 0 : 1;
}

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../src/recapture_compare.h"

static void fill(BYTE *pixels, UINT width, UINT height, BYTE value) {
    for (UINT i = 0; i < width * height; ++i) {
        pixels[i * 4u + 0u] = value;
        pixels[i * 4u + 1u] = value;
        pixels[i * 4u + 2u] = value;
        pixels[i * 4u + 3u] = 255;
    }
}

int main(void) {
    BYTE before_pixels[4u * 4u * 4u];
    BYTE after_pixels[4u * 4u * 4u];
    fill(before_pixels, 4, 4, 0);
    memcpy(after_pixels, before_pixels, sizeof(after_pixels));
    GoldenImage before = {before_pixels, 4, 4, 16};
    GoldenImage after = {after_pixels, 4, 4, 16};
    GoldenRecaptureComparison comparison = {0};
    int failed = !golden_compare_recapture(&before, &after, &comparison) ||
                 comparison.size_changed || comparison.difference_warning ||
                 comparison.difference_percent != 0.0;

    fill(after_pixels, 4, 4, 255);
    if (!failed && (!golden_compare_recapture(&before, &after, &comparison) ||
                    comparison.difference_percent != 100.0 ||
                    !comparison.difference_warning)) failed = 1;

    BYTE large_pixels[8u * 8u * 4u];
    fill(large_pixels, 8, 8, 0);
    GoldenImage large = {large_pixels, 8, 8, 32};
    if (!failed && (!golden_compare_recapture(&before, &large, &comparison) ||
                    !comparison.size_changed ||
                    comparison.difference_percent != 0.0 ||
                    comparison.difference_warning)) failed = 1;

    GoldenImage invalid = {0};
    if (!failed && golden_compare_recapture(
            &invalid, &after, &comparison)) failed = 1;

    if (failed) {
        fprintf(stderr, "Goldens recapture comparison test failed\n");
        return 1;
    }
    puts("All Goldens recapture comparison tests passed.");
    return 0;
}

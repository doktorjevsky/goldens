#include "recapture_compare.h"

#include <stdint.h>

#define COMPARISON_SAMPLES 64u

static const BYTE *sample_pixel(const GoldenImage *image, UINT sample_x,
                                UINT sample_y, UINT samples_x,
                                UINT samples_y) {
    uint64_t x = ((uint64_t)sample_x * 2u + 1u) * image->width /
                 ((uint64_t)samples_x * 2u);
    uint64_t y = ((uint64_t)sample_y * 2u + 1u) * image->height /
                 ((uint64_t)samples_y * 2u);
    if (x >= image->width) x = image->width - 1u;
    if (y >= image->height) y = image->height - 1u;
    return image->pixels + y * image->stride + x * 4u;
}

BOOL golden_compare_recapture(const GoldenImage *before,
                              const GoldenImage *after,
                              GoldenRecaptureComparison *comparison) {
    if (!before || !after || !comparison || !before->pixels ||
        !after->pixels || !before->width || !before->height ||
        !after->width || !after->height ||
        before->width > UINT32_MAX / 4u ||
        after->width > UINT32_MAX / 4u ||
        before->stride < before->width * 4u ||
        after->stride < after->width * 4u) return FALSE;

    UINT samples_x = min(COMPARISON_SAMPLES,
                         min(before->width, after->width));
    UINT samples_y = min(COMPARISON_SAMPLES,
                         min(before->height, after->height));
    uint64_t difference = 0;
    for (UINT y = 0; y < samples_y; ++y) {
        for (UINT x = 0; x < samples_x; ++x) {
            const BYTE *left = sample_pixel(before, x, y,
                                             samples_x, samples_y);
            const BYTE *right = sample_pixel(after, x, y,
                                              samples_x, samples_y);
            for (int channel = 0; channel < 3; ++channel) {
                int delta = (int)left[channel] - (int)right[channel];
                difference += (uint64_t)(delta < 0 ? -delta : delta);
            }
        }
    }
    uint64_t maximum = (uint64_t)samples_x * samples_y * 3u * 255u;
    comparison->size_changed = before->width != after->width ||
                               before->height != after->height;
    comparison->difference_percent =
        maximum ? (double)difference * 100.0 / (double)maximum : 0.0;
    comparison->difference_warning = comparison->difference_percent >
        GOLDEN_RECAPTURE_DIFFERENCE_WARNING_PERCENT;
    return TRUE;
}

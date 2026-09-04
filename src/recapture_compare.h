#ifndef GOLDENS_RECAPTURE_COMPARE_H
#define GOLDENS_RECAPTURE_COMPARE_H

#include "image_io.h"

#define GOLDEN_RECAPTURE_DIFFERENCE_WARNING_PERCENT 20.0

typedef struct {
    BOOL size_changed;
    double difference_percent;
    BOOL difference_warning;
} GoldenRecaptureComparison;

BOOL golden_compare_recapture(const GoldenImage *before,
                              const GoldenImage *after,
                              GoldenRecaptureComparison *comparison);

#endif

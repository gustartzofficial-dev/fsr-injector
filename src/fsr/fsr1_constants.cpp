#include "fsr/fsr1_constants.h"

// Pull in AMD's portable header pair in CPU mode. A_CPU gives us the AU1/AF1
// typedefs plus FsrEasuCon/FsrRcasCon as static inline C functions, so the
// exact same header that runs in the shaders computes the constants here --
// no chance of CPU/GPU constant-layout drift.
// NOTE: ffx_a.h uses unqualified C math functions, so the C headers must come
// first (this is the documented AMD usage pattern).
#include <math.h>
#include <string.h>
#include <stdint.h>
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

namespace fsr1 {

void easu_constants(EasuConstants& out,
                    float input_width, float input_height,
                    float output_width, float output_height) {
    FsrEasuCon(&out.con[0], &out.con[4], &out.con[8], &out.con[12],
               input_width, input_height,   // viewport actually rendered
               input_width, input_height,   // full size of the source texture
               output_width, output_height);
}

void rcas_constants(RcasConstants& out, float sharpness_stops) {
    if (sharpness_stops < 0.0f) sharpness_stops = 0.0f;
    if (sharpness_stops > 2.5f) sharpness_stops = 2.5f;
    FsrRcasCon(&out.con[0], sharpness_stops);
}

float slider_to_stops(float slider01) {
    if (slider01 < 0.0f) slider01 = 0.0f;
    if (slider01 > 1.0f) slider01 = 1.0f;
    // slider 1.0 -> 0 stops (max sharpen), slider 0.0 -> 2 stops (mild).
    return (1.0f - slider01) * 2.0f;
}

} // namespace fsr1

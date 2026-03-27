#include "pipeline/tbc_resample.h"
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// tbc_resample — Port of cuVHS k_tbc_resample kernel to single-field CPU.
//
// Was one-thread-per-pixel on GPU; becomes nested loop over (line, col).
// Pure transform: (demod, linelocs, fmt) → tbc_luma output buffer.
//
// [NTSC-SPECIFIC] active_line_start, output_field_lines, output_line_len,
// and the IRE-to-Hz mapping are all NTSC VHS SP values from VideoFormat.
// ---------------------------------------------------------------------------

void tbc_resample(const double* demod,
                  const double* linelocs,
                  uint16_t* tbc_luma,
                  int total_demod_samples,
                  const VideoFormat& fmt)
{
    const int out_lines   = fmt.output_field_lines;  // [NTSC-SPECIFIC] 263
    const int out_cols    = fmt.output_line_len;      // [NTSC-SPECIFIC] 910
    const int line_start  = fmt.active_line_start;    // [NTSC-SPECIFIC] 10
    const int lpf         = fmt.lines_per_frame;      // [NTSC-SPECIFIC] 525

    // [NTSC-SPECIFIC] IRE-to-Hz and output scaling constants
    const double ire0         = fmt.ire0;
    const double hz_ire       = fmt.hz_ire;
    const double vsync_ire    = fmt.vsync_ire;
    const double output_zero  = fmt.output_zero;
    const double output_scale = fmt.output_scale;

    for (int out_line = 0; out_line < out_lines; out_line++) {
        // Map output line to linelocs indices
        int ll_line = out_line + line_start;
        int ll_next = ll_line + 1;

        uint16_t* row = tbc_luma + out_line * out_cols;

        // Bounds check: need both ll_line and ll_next within linelocs
        if (ll_next >= lpf) {
            // Past end of known line positions — fill with blanking level
            uint16_t blank = static_cast<uint16_t>(std::lround(output_zero));
            for (int col = 0; col < out_cols; col++) {
                row[col] = blank;
            }
            continue;
        }

        double line_begin = linelocs[ll_line];
        double line_end   = linelocs[ll_next];

        for (int out_col = 0; out_col < out_cols; out_col++) {
            // Interpolate input position within line
            double frac  = static_cast<double>(out_col) / static_cast<double>(out_cols);
            double coord = line_begin + frac * (line_end - line_begin);

            // Catmull-Rom cubic interpolation
            int ci    = static_cast<int>(coord);
            double x  = coord - ci;

            if (ci < 1 || ci + 2 >= total_demod_samples) {
                // Out of bounds — output blanking level
                row[out_col] = static_cast<uint16_t>(std::lround(output_zero));
                continue;
            }

            double p0 = demod[ci - 1];
            double p1 = demod[ci];
            double p2 = demod[ci + 1];
            double p3 = demod[ci + 2];

            // Catmull-Rom coefficients
            double a = p2 - p0;
            double b = 2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3;
            double c = 3.0 * (p1 - p2) + p3 - p0;

            double hz_value = p1 + 0.5 * x * (a + x * (b + x * c));

            // Convert Hz → IRE → uint16
            double ire     = (hz_value - ire0) / hz_ire - vsync_ire;
            double out_val = ire * output_scale + output_zero;

            // Clamp to uint16 range and store
            out_val = std::clamp(out_val, 0.0, 65535.0);
            row[out_col] = static_cast<uint16_t>(std::lround(out_val));
        }
    }
}

#include "pipeline/sync_pulses.h"

// Sequential threshold crossing detector for one field.
// Scans demod05 (sync signal in Hz) and records pulses where the signal
// drops below pulse_threshold_hz. Matches Python findpulses_numba_raw().

int sync_pulses(const double* demod05,
                int* pulse_starts,
                int* pulse_lengths,
                int samples_per_field,
                const VideoFormat& fmt)
{
    double threshold = fmt.pulse_threshold_hz;

    bool in_pulse = (demod05[0] <= threshold);
    int cur_start = 0;
    int count = 0;

    for (int i = 0; i < samples_per_field; i++) {
        double val = demod05[i];

        if (in_pulse) {
            if (val > threshold) {
                // Rising edge: end of pulse
                int length = i - cur_start;
                // Record pulse if length > 0 and not at sample 0
                // (Python: "if cur_start != 0" — skip pulse starting at pos 0)
                if (length > 0 && cur_start != 0 && count < MAX_PULSES) {
                    pulse_starts[count]  = cur_start;   // 0-based within field
                    pulse_lengths[count] = length;
                    count++;
                }
                in_pulse = false;
            }
        } else {
            if (val <= threshold) {
                cur_start = i;
                in_pulse = true;
            }
        }
    }

    return count;
}

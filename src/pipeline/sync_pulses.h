#pragma once
#include "format/video_format.h"

// Maximum pulses per field. NTSC has ~263 HSYNCs + ~18 EQ/VSYNC = ~281.
// 800 is generous enough for any real signal including noise pulses.
static const int MAX_PULSES = 800;

// Find sync pulses in one field of sync signal (demod05, in Hz).
// Returns number of pulses found.
//
// Sequential threshold crossing detector matching Python findpulses_numba_raw().
// pulse_starts/pulse_lengths are 0-based positions within this field.
int sync_pulses(const double* demod05,
                int* pulse_starts,
                int* pulse_lengths,
                int samples_per_field,
                const VideoFormat& fmt);

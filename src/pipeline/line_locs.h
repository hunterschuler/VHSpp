#pragma once
#include "format/video_format.h"
#include "pipeline/sync_pulses.h"

// Pulse type codes (matches Python lddecode/core.py)
enum PulseType : int { PULSE_HSYNC = 0, PULSE_EQ1 = 1, PULSE_VSYNC = 2, PULSE_EQ2 = 3 };

// Classify pulses and compute line locations for one field.
//
// Four-phase algorithm ported from cuVHS k_compute_linelocs:
//   Phase 0: Adaptive classification (measure mean HSYNC width, shift thresholds)
//   Phase 1: VBLANK state machine (find VSYNC reference pulse)
//   Phase 1b: Field parity detection from pulse spacing
//   Phase 2: Mean line length from longest HSYNC run
//   Phase 3: Line assignment (snap to nearest HSYNC within tolerance)
//
// Output:
//   linelocs[lines_per_frame] = sample position of each line start (0-based in field)
//   is_first_field: 1=first, 0=second, -1=unknown
void line_locs(const int* pulse_starts,
               const int* pulse_lengths,
               int num_pulses,
               double* linelocs,          // [lines_per_frame] output
               int* is_first_field,       // output: 1, 0, or -1
               int samples_per_field,
               const VideoFormat& fmt);

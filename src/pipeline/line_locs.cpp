#include "pipeline/line_locs.h"
#include <cmath>

// Classify pulses and compute line locations for one field.
// Ported from cuVHS k_compute_linelocs — already per-field sequential.

void line_locs(const int* pulse_starts,
               const int* pulse_lengths,
               int num_pulses,
               double* linelocs,
               int* is_first_field,
               int samples_per_field,
               const VideoFormat& fmt)
{
    int npc = num_pulses;
    int npc_clamped = (npc < MAX_PULSES) ? npc : MAX_PULSES;
    double inlinelen = (double)fmt.samples_per_line;

    double hsync_nominal = fmt.hsync_width;
    double eq_nominal    = fmt.eq_pulse_width;
    double vsync_nominal = fmt.vsync_width;
    double tol_samples   = 0.5e-6 * fmt.sample_rate;

    // -----------------------------------------------------------------
    // Phase 0: Adaptive pulse classification
    //
    // Different VHS recordings have different absolute pulse widths.
    // Measure mean HSYNC width per field and shift all thresholds.
    // Matches Python vhs-decode core.py get_timings().
    // -----------------------------------------------------------------

    // Generous first pass: find HSYNC-like pulses (±1.75µs / +2µs, matching Python)
    double generous_hsync_min = hsync_nominal - 3.5 * tol_samples;  // -1.75µs
    double generous_hsync_max = hsync_nominal + 4.0 * tol_samples;  // +2.0µs

    double hsync_sum = 0.0;
    int hsync_count = 0;
    for (int i = 0; i < npc_clamped; i++) {
        double len = (double)pulse_lengths[i];
        if (len >= generous_hsync_min && len <= generous_hsync_max) {
            hsync_sum += len;
            hsync_count++;
        }
    }

    double hsync_median_est = (hsync_count > 0) ? (hsync_sum / hsync_count) : hsync_nominal;
    double hsync_offset = hsync_median_est - hsync_nominal;

    // Re-classify with shifted thresholds
    double a_hsync_min = hsync_nominal + hsync_offset - tol_samples;
    double a_hsync_max = hsync_nominal + hsync_offset + tol_samples;
    double a_eq_min    = eq_nominal + hsync_offset - tol_samples;
    double a_eq_max    = eq_nominal + hsync_offset + tol_samples;
    double a_vsync_min = (vsync_nominal + hsync_offset) * 0.5;
    double a_vsync_max = vsync_nominal + hsync_offset + 2.0 * tol_samples;

    int local_types[MAX_PULSES];
    for (int i = 0; i < npc_clamped; i++) {
        double len = (double)pulse_lengths[i];
        if (len >= a_vsync_min && len <= a_vsync_max) {
            local_types[i] = PULSE_VSYNC;
        } else if (len >= a_hsync_min && len <= a_hsync_max) {
            local_types[i] = PULSE_HSYNC;
        } else if (len >= a_eq_min && len <= a_eq_max) {
            local_types[i] = PULSE_EQ1;  // EQ1 or EQ2 — refined by state machine
        } else {
            local_types[i] = PULSE_HSYNC;  // unclassified → HSYNC fallback
        }
    }

    // -----------------------------------------------------------------
    // Phase 1: VBLANK state machine
    //
    // Track state transitions: HSYNC -> EQ1 -> VSYNC -> EQ2 -> found ref_pulse
    // Record the index of the first HSYNC after the VSYNC/EQ2 section.
    // -----------------------------------------------------------------

    int ref_pulse_idx = -1;
    int last_hsync_idx = -1;
    int first_eq1_idx = -1;
    int last_eq2_idx = -1;
    int state = -1;  // -1 = initial

    for (int i = 0; i < npc_clamped; i++) {
        int t = local_types[i];

        if (state == -1) {
            if (t == PULSE_HSYNC) { state = PULSE_HSYNC; last_hsync_idx = i; }
        } else if (state == PULSE_HSYNC) {
            if (t == PULSE_HSYNC) {
                last_hsync_idx = i;
            } else if (t == PULSE_EQ1) {
                state = PULSE_EQ1;
                first_eq1_idx = i;
            } else if (t == PULSE_VSYNC) {
                state = PULSE_VSYNC;
            }
        } else if (state == PULSE_EQ1) {
            if (t == PULSE_EQ1) {
                // stay
            } else if (t == PULSE_VSYNC) {
                state = PULSE_VSYNC;
            } else if (t == PULSE_HSYNC) {
                // false alarm, back to HSYNC
                state = PULSE_HSYNC;
                last_hsync_idx = i;
                first_eq1_idx = -1;
            }
        } else if (state == PULSE_VSYNC) {
            if (t == PULSE_VSYNC) {
                // stay
            } else if (t == PULSE_EQ1) {
                // VSYNC -> EQ2 transition
                state = PULSE_EQ2;
                last_eq2_idx = i;
            } else if (t == PULSE_HSYNC) {
                // Direct VSYNC -> HSYNC (unusual, but accept it)
                ref_pulse_idx = i;
                break;
            }
        } else if (state == PULSE_EQ2) {
            if (t == PULSE_EQ1) {
                // stay in EQ2
                last_eq2_idx = i;
            } else if (t == PULSE_HSYNC) {
                if (last_eq2_idx < 0) last_eq2_idx = i - 1;  // fallback
                ref_pulse_idx = i;
                break;
            }
        }
    }

    // -----------------------------------------------------------------
    // Phase 1b: [NTSC-SPECIFIC] Field parity detection from VSYNC pulse pattern
    //
    // NTSC first field: HSYNC→EQ1 spacing > 0.75 lines
    // NTSC second field: HSYNC→EQ1 spacing < 0.75 lines
    // Only trust when full vblank was found (ref_pulse_idx >= 0).
    // -----------------------------------------------------------------

    int field_parity = -1;

    // Try entry spacing first: last HSYNC before vblank → first EQ1
    if (ref_pulse_idx >= 0 && last_hsync_idx >= 0 && first_eq1_idx >= 0) {
        double entry_spacing = (double)(pulse_starts[first_eq1_idx] - pulse_starts[last_hsync_idx]);
        double entry_lines = entry_spacing / inlinelen;
        // [NTSC-SPECIFIC] parity from entry spacing
        field_parity = (entry_lines > 0.75) ? 1 : 0;
    }

    // Fallback: exit spacing — last EQ2 pulse → first HSYNC after vblank
    if (field_parity < 0 && ref_pulse_idx >= 0 && last_eq2_idx >= 0) {
        double exit_spacing = (double)(pulse_starts[ref_pulse_idx] - pulse_starts[last_eq2_idx]);
        double exit_lines = exit_spacing / inlinelen;
        // [NTSC-SPECIFIC] parity from exit spacing
        field_parity = (exit_lines > 0.75) ? 1 : 0;
    }

    *is_first_field = field_parity;

    // -----------------------------------------------------------------
    // Phase 2: Mean line length from longest consecutive HSYNC run
    //
    // Average HSYNC-to-HSYNC spacings within the best run.
    // Only include spacings within 5% of nominal (matches Python).
    // -----------------------------------------------------------------

    int best_run_start = -1, best_run_len = 0;
    int cur_run_start = -1, cur_run_len = 0;

    for (int i = 0; i < npc_clamped; i++) {
        if (local_types[i] == PULSE_HSYNC) {
            if (cur_run_start == -1) {
                cur_run_start = i;
                cur_run_len = 1;
            } else {
                cur_run_len++;
            }
        } else {
            if (cur_run_len > best_run_len) {
                best_run_start = cur_run_start;
                best_run_len = cur_run_len;
            }
            cur_run_start = -1;
            cur_run_len = 0;
        }
    }
    if (cur_run_len > best_run_len) {
        best_run_start = cur_run_start;
        best_run_len = cur_run_len;
    }

    double meanlinelen = inlinelen;  // fallback
    if (best_run_len >= 2) {
        double sum = 0.0;
        int count = 0;
        for (int i = best_run_start + 1; i < best_run_start + best_run_len; i++) {
            double spacing = (double)(pulse_starts[i] - pulse_starts[i - 1]);
            if (spacing >= inlinelen * 0.95 && spacing <= inlinelen * 1.05) {
                sum += spacing;
                count++;
            }
        }
        if (count > 0) {
            meanlinelen = sum / count;
        }
    }

    // -----------------------------------------------------------------
    // Phase 3: Line assignment
    //
    // Determine reference position and line number, then assign each
    // output line to the nearest HSYNC pulse within tolerance.
    // -----------------------------------------------------------------

    double ref_position;
    double ref_line;

    if (ref_pulse_idx >= 0) {
        ref_position = (double)pulse_starts[ref_pulse_idx];
        ref_line = 19.0;  // cuVHS calibration constant — aligns with Python vhs-decode

        // If the state machine found VSYNC in the second half of the pulse list,
        // it likely locked onto the NEXT field's VSYNC (the damaged leading sync
        // was skipped). Subtract one field's worth of samples to project back to
        // where this field's VSYNC should have been. Recovers position to within
        // ~150 samples — well within hsync refinement range.
        //
        // CRITICAL GUARD: corrected_ref > 0. Field 0 starts at file offset 0,
        // before the first VSYNC. Its ref_pulse_idx is legitimately in the middle
        // of the pulse list (~155 for NTSC). Without this guard, the subtraction
        // makes ref_position negative, corrupting field 0's line positions — and
        // poisoning all downstream state (chroma track, phase, etc.).
        if (ref_pulse_idx > npc_clamped / 2) {
            double corrected = ref_position - (double)samples_per_field;
            if (corrected > 0.0) {
                ref_position = corrected;
            }
        }
    } else {
        // No VSYNC found: use first HSYNC pulse as rough reference
        ref_pulse_idx = 0;
        for (int i = 0; i < npc_clamped; i++) {
            if (local_types[i] == PULSE_HSYNC) {
                ref_pulse_idx = i;
                break;
            }
        }
        ref_position = (double)pulse_starts[ref_pulse_idx];
        ref_line = 0.0;
    }

    // Build compact list of HSYNC-only pulse starts for matching
    int hsync_starts[MAX_PULSES];
    int num_hsyncs = 0;
    for (int i = 0; i < npc_clamped && num_hsyncs < MAX_PULSES; i++) {
        if (local_types[i] == PULSE_HSYNC) {
            hsync_starts[num_hsyncs++] = pulse_starts[i];
        }
    }

    double max_allowed_distance = meanlinelen / 1.5;
    int cur_hsync = 0;  // monotonic cursor into hsync_starts

    for (int line = 0; line < fmt.lines_per_frame; line++) {
        double expected = ref_position + meanlinelen * (line - ref_line);
        linelocs[line] = expected;

        if (cur_hsync >= num_hsyncs) continue;

        // Search forward for nearest HSYNC within tolerance
        double best_dist = max_allowed_distance;
        int best_pos = -1;
        int search = cur_hsync;

        while (search < num_hsyncs) {
            double dist = fabs((double)hsync_starts[search] - expected);
            if (dist <= best_dist) {
                best_dist = dist;
                best_pos = hsync_starts[search];
                cur_hsync = search;
            }
            // If next pulse is farther away, we've passed the minimum
            if (search + 1 < num_hsyncs) {
                double next_dist = fabs((double)hsync_starts[search + 1] - expected);
                if (next_dist > dist) break;
            } else {
                break;
            }
            search++;
        }

        if (best_pos >= 0) {
            linelocs[line] = (double)best_pos;
            cur_hsync++;  // consume this pulse
        }
    }
}

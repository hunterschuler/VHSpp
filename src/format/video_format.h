#pragma once
#include <cstdint>

enum class VideoSystem { NTSC, PAL };
enum class InputFormat { U8, S16, U16 };

const char* input_format_name(InputFormat fmt);
int input_format_bytes_per_sample(InputFormat fmt);

// All the timing/frequency constants needed by the pipeline.
// Derived at construction from system + sample_rate.
//
// =========================================================================
// [NTSC-SPECIFIC] FORMAT CONSTANTS
//
// *** NTSC IS THE ONLY SUPPORTED SYSTEM. ***
// All values below are NTSC VHS SP. When adding PAL/SECAM/etc., add a new
// branch in the constructor (video_format.cpp) and validate end-to-end.
// Every field tagged [NTSC-SPECIFIC] will need a system-specific value.
// =========================================================================
struct VideoFormat {
    VideoSystem system;
    double sample_rate;          // Hz (e.g., 28e6)

    // Line and field geometry
    int    lines_per_frame;      // [NTSC-SPECIFIC] 525
    int    lines_per_field;      // [NTSC-SPECIFIC] 262/263
    double line_rate;            // [NTSC-SPECIFIC] 15734.264 Hz
    double field_rate;           // [NTSC-SPECIFIC] 59.94 Hz
    double frame_rate;           // [NTSC-SPECIFIC] 29.97 Hz

    // Derived sample counts (computed from above)
    int    samples_per_line;     // at capture sample rate
    int    samples_per_field;    // approximate

    // Sync timing (in samples at capture rate)
    double hsync_width;          // ~4.7 us
    double vsync_width;          // [NTSC-SPECIFIC] ~27.1 us
    double eq_pulse_width;       // ~2.3 us
    double bp_width;             // back porch

    // Carrier frequencies (Hz) — VHS format dependent
    double luma_carrier;         // [NTSC-SPECIFIC] 3.9 MHz
    double chroma_under;         // [NTSC-SPECIFIC] ~629 kHz (40×fH)
    double fsc;                  // [NTSC-SPECIFIC] 3.579545 MHz

    // IRE-to-Hz mapping (after FM demod, signal is in Hz)
    // These define the VHS FM deviation. Different for every tape format AND system.
    double ire0;                 // [NTSC-SPECIFIC] ~3,685,714 Hz
    double hz_ire;               // [NTSC-SPECIFIC] ~7142.857 Hz/IRE
    double vsync_ire;            // [NTSC-SPECIFIC] -40 IRE

    // Derived sync detection levels (Hz)
    double sync_tip_hz;          // frequency at sync tip
    double pulse_threshold_hz;   // threshold for pulse detection (-20 IRE midpoint)

    // TBC output geometry (fixed at 4*fsc)
    double output_rate;          // 4 * fsc
    int    output_line_len;      // [NTSC-SPECIFIC] 910
    int    output_field_lines;   // [NTSC-SPECIFIC] 263

    // Vblank structure
    int    num_eq_pulses;        // [NTSC-SPECIFIC] 6
    int    field_lines_first;    // [NTSC-SPECIFIC] 263
    int    field_lines_second;   // [NTSC-SPECIFIC] 262

    // TBC output scaling (Hz -> uint16, ld-tools compatible)
    double output_zero;          // [NTSC-SPECIFIC] 1024
    double output_scale;         // [NTSC-SPECIFIC] (51200-1024)/(100-vsync_ire)
    int    active_line_start;    // [NTSC-SPECIFIC] 10

    // Burst reference for ACC normalization
    double burst_abs_ref;        // [NTSC-SPECIFIC] 4416 (VHS SP)
    double burst_start_us;       // [NTSC-SPECIFIC] 5.3 us
    double burst_end_us;         // [NTSC-SPECIFIC] 7.8 us

    VideoFormat(VideoSystem sys, double sample_rate_mhz);
    void print_info() const;
};

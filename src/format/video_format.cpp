#include "format/video_format.h"
#include <cstdio>
#include <cmath>

const char* input_format_name(InputFormat fmt) {
    switch (fmt) {
        case InputFormat::U8:  return "u8";
        case InputFormat::S16: return "s16";
        case InputFormat::U16: return "u16";
    }
    return "unknown";
}

int input_format_bytes_per_sample(InputFormat fmt) {
    switch (fmt) {
        case InputFormat::U8:  return 1;
        case InputFormat::S16: return 2;
        case InputFormat::U16: return 2;
    }
    return 1;
}

VideoFormat::VideoFormat(VideoSystem sys, double sample_rate_mhz)
    : system(sys), sample_rate(sample_rate_mhz * 1e6)
{
    // =========================================================================
    // [NTSC-SPECIFIC] SYSTEM CONSTANTS — NTSC ONLY
    //
    // Only NTSC VHS SP is implemented. When adding PAL/SECAM/etc., add an
    // else-if branch below with validated constants from real tape captures.
    //
    // The IRE-to-Hz mapping (hz_ire, ire0, vsync_ire) also varies by tape
    // format (VHS SP, VHS LP, Beta, Video8, etc.) — currently hardcoded for
    // VHS SP. A future tape_format parameter could select these.
    // =========================================================================
    // [NTSC-SPECIFIC] NTSC is the only supported system right now.
    // When adding PAL/SECAM/etc., add an else-if branch here with validated constants.
    if (sys != VideoSystem::NTSC) {
        fprintf(stderr, "ERROR: Only NTSC is supported. PAL/other systems are not yet implemented.\n");
        // Zero-init everything so it's obvious if someone tries to use it
        lines_per_frame = 0; lines_per_field = 0; line_rate = 0; field_rate = 0;
        frame_rate = 0; fsc = 0; chroma_under = 0; luma_carrier = 0;
        burst_abs_ref = 0; burst_start_us = 0; burst_end_us = 0;
        output_line_len = 0; output_field_lines = 0; num_eq_pulses = 0;
        field_lines_first = 0; field_lines_second = 0;
        hz_ire = 0; ire0 = 0; vsync_ire = 0;
        sync_tip_hz = 0; pulse_threshold_hz = 0;
        output_zero = 0; output_scale = 0; active_line_start = 0;
        output_rate = 0; samples_per_line = 0; samples_per_field = 0;
        hsync_width = 0; eq_pulse_width = 0; bp_width = 0; vsync_width = 0;
        return;
    }

    // --- [NTSC-SPECIFIC] Line/field geometry (ITU-R BT.601) ---
    lines_per_frame  = 525;
    lines_per_field  = 263;    // alternates 262/263
    line_rate        = 15734.264;
    field_rate       = 59.94;
    frame_rate       = 29.97;

    // --- [NTSC-SPECIFIC] Carrier/subcarrier frequencies ---
    fsc              = 3579545.0;                       // NTSC colorburst
    chroma_under     = (525.0 * (30.0 / 1.001)) * 40.0;  // 629370.6 Hz (40 × fH)
    luma_carrier     = 3900000.0;                       // VHS NTSC FM carrier center

    // --- [NTSC-SPECIFIC] Burst reference ---
    burst_abs_ref    = 4416.0;   // VHS SP burst amplitude target
    burst_start_us   = 5.3;      // burst window start (us from line start)
    burst_end_us     = 7.8;      // burst window end

    // --- [NTSC-SPECIFIC] TBC output geometry ---
    output_line_len    = 910;    // samples per line at 4×fsc
    output_field_lines = 263;
    num_eq_pulses      = 6;      // EQ pulses per vblank section
    field_lines_first  = 263;
    field_lines_second = 262;

    // --- [NTSC-SPECIFIC] VHS NTSC SP IRE mapping (from vhsdecode format_defs/vhs.py) ---
    // These three values define the FM deviation for VHS NTSC SP tapes.
    // Different tape speeds (LP) or formats (Beta, Video8) have different values.
    hz_ire    = 1e6 / 140.0;                    // ~7142.857 Hz/IRE
    ire0      = 4.4e6 - (hz_ire * 100.0);       // ~3,685,714 Hz at 0 IRE
    vsync_ire = -40.0;                           // sync tip at -40 IRE

    // Derived sync levels
    sync_tip_hz        = ire0 + hz_ire * vsync_ire;
    pulse_threshold_hz = ire0 + hz_ire * (-20.0);    // -20 IRE: halfway between sync and blanking

    // --- [NTSC-SPECIFIC] TBC output scaling (ld-tools compatible uint16 range) ---
    output_zero  = 1024.0;
    output_scale = (51200.0 - 1024.0) / (100.0 - vsync_ire);
    active_line_start = 10;

    output_rate = 4.0 * fsc;

    samples_per_line  = static_cast<int>(round(sample_rate / line_rate));
    samples_per_field = samples_per_line * lines_per_field;

    // Sync pulse widths in samples
    hsync_width    = 4.7e-6 * sample_rate;
    eq_pulse_width = 2.3e-6 * sample_rate;
    bp_width       = 1.5e-6 * sample_rate;

    // --- [NTSC-SPECIFIC] VSYNC width ---
    vsync_width = 27.1e-6 * sample_rate;
}

void VideoFormat::print_info() const {
    const char* sys_name = (system == VideoSystem::NTSC) ? "NTSC" : "PAL";
    fprintf(stderr, "Format: %s @ %.1f MHz capture rate\n", sys_name, sample_rate / 1e6);
    fprintf(stderr, "  Lines/frame: %d  Line rate: %.3f Hz\n", lines_per_frame, line_rate);
    fprintf(stderr, "  Samples/line: %d  Samples/field: %d\n", samples_per_line, samples_per_field);
    fprintf(stderr, "  Output: %d samples/line @ %.6f MHz (4*fsc)\n",
            output_line_len, output_rate / 1e6);
    fprintf(stderr, "  Chroma-under: %.0f Hz  Fsc: %.0f Hz\n", chroma_under, fsc);
    fprintf(stderr, "  IRE: 0=%.0f Hz  sync_tip=%.0f Hz  threshold=%.0f Hz  (%.1f Hz/IRE)\n",
            ire0, sync_tip_hz, pulse_threshold_hz, hz_ire);
}

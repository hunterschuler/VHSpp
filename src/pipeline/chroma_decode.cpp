#include "pipeline/chroma_decode.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const double PI  = 3.14159265358979323846;
static const double TAU = 2.0 * PI;

// ---------------------------------------------------------------------------
// FFT size helpers -- find next 7-smooth number for efficient FFTs
// (same as fm_demod -- redefined static to keep chroma_decode self-contained)
// ---------------------------------------------------------------------------

static bool is_7smooth(int n)
{
    while (n % 2 == 0) n /= 2;
    while (n % 3 == 0) n /= 3;
    while (n % 5 == 0) n /= 5;
    while (n % 7 == 0) n /= 7;
    return n == 1;
}

static int next_fft_size(int n)
{
    while (!is_7smooth(n)) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Butterworth filter magnitude responses
// (same as fm_demod -- redefined static)
// ---------------------------------------------------------------------------

static double butter_lpf_mag(double f, double fc, int order)
{
    double ratio = f / fc;
    double r2n = 1.0;
    for (int i = 0; i < order; ++i) r2n *= ratio * ratio;
    return 1.0 / std::sqrt(1.0 + r2n);
}

static double butter_hpf_mag(double f, double fc, int order)
{
    if (f == 0.0) return 0.0;
    double ratio = fc / f;
    double r2n = 1.0;
    for (int i = 0; i < order; ++i) r2n *= ratio * ratio;
    return 1.0 / std::sqrt(1.0 + r2n);
}

static double butter_bpf_mag(double f, double f_low, double f_high, int order)
{
    return butter_hpf_mag(f, f_low, order) * butter_lpf_mag(f, f_high, order);
}

// ---------------------------------------------------------------------------
// [NTSC-SPECIFIC] NTSC 4-frame (8-field) phase rotation table
//
// VHS NTSC color-under uses a phase rotation that repeats every 4 frames
// (8 fields). The table encodes the relationship between field parity,
// burst phase quadrant, inter-field quadrant delta, the resulting phase ID
// (1-4), and the heterodyne phase offset for that field.
//
// Each entry: {is_first_field, quadrant, delta_from_prev, phase_id, het_offset}
// Quadrant: 0-3 representing burst phase in 90-degree increments.
// Delta: difference in quadrant from previous field (mod 4).
// Phase ID: 1-4, the NTSC colorburst phase identity for TBC metadata.
// Het offset: base heterodyne phase offset (in quadrants) for this field.
// ---------------------------------------------------------------------------

struct NTSCPhaseEntry {
    int is_first;
    int quadrant;
    int delta;
    int phase_id;
    int offset;
};

// [NTSC-SPECIFIC] Complete 16-entry phase table covering the 8-field cycle.
// Two entries per field position: one for is_first=1, one for is_first=0.
// Derived from cuVHS chroma kernel empirical calibration.
static const NTSCPhaseEntry ntsc_phase_table[] = {
    // Frame 1
    { 1, 0,  0, 1, 0 },  // field 1 (first)
    { 0, 1,  1, 2, 1 },  // field 2 (second)
    // Frame 2
    { 1, 2,  1, 3, 2 },  // field 3 (first)
    { 0, 3,  1, 4, 3 },  // field 4 (second)
    // Frame 3
    { 1, 2, -1, 1, 2 },  // field 5 (first)
    { 0, 3,  1, 2, 3 },  // field 6 (second)
    // Frame 4
    { 1, 0,  1, 3, 0 },  // field 7 (first)
    { 0, 1,  1, 4, 1 },  // field 8 (second)
    // Repeat with opposite parity (for robustness -- same cycle shifted)
    { 0, 0,  0, 1, 0 },
    { 1, 1,  1, 2, 1 },
    { 0, 2,  1, 3, 2 },
    { 1, 3,  1, 4, 3 },
    { 0, 2, -1, 1, 2 },
    { 1, 3,  1, 2, 3 },
    { 0, 0,  1, 3, 0 },
    { 1, 1,  1, 4, 1 },
};

static const int NTSC_PHASE_TABLE_SIZE = 16;

struct NTSCPhaseResult {
    int phase_id;
    int offset;
    bool found;
};

// Look up NTSC phase from field parity and burst quadrant + delta.
static NTSCPhaseResult lookup_ntsc_phase(int is_first, int quadrant, int delta)
{
    // Normalize delta to [-2, 2] range
    int d = ((delta + 2) % 4) - 2;
    if (d < -2) d += 4;

    for (int i = 0; i < NTSC_PHASE_TABLE_SIZE; ++i) {
        if (ntsc_phase_table[i].is_first == is_first &&
            ntsc_phase_table[i].quadrant == quadrant &&
            ntsc_phase_table[i].delta == d) {
            NTSCPhaseResult r;
            r.phase_id = ntsc_phase_table[i].phase_id;
            r.offset   = ntsc_phase_table[i].offset;
            r.found    = true;
            return r;
        }
    }

    // Fallback: use quadrant directly
    NTSCPhaseResult r;
    r.phase_id = (quadrant % 4) + 1;
    r.offset   = quadrant % 4;
    r.found    = false;
    return r;
}

// ---------------------------------------------------------------------------
// Burst measurement helpers (operate on demodulated chroma lines)
// ---------------------------------------------------------------------------

// Measure average burst phase quadrant across active lines of one field.
// chroma_lines: array of doubles [output_field_lines × line_len]
// line_len: number of samples per output line
// burst_start, burst_end: sample indices within each line for burst window
// Returns quadrant 0-3.
static int measure_burst_phase(const double* chroma_lines,
                               int output_field_lines, int line_len,
                               int burst_start, int burst_end,
                               double fsc, double output_rate)
{
    // Accumulate burst phase across active lines (skip first 10 and last 5)
    const int first_active = 10;
    const int last_active  = output_field_lines - 5;
    double sum_sin = 0.0;
    double sum_cos = 0.0;
    int count = 0;

    for (int line = first_active; line < last_active; ++line) {
        const double* row = chroma_lines + line * line_len;
        for (int col = burst_start; col < burst_end; ++col) {
            // Reference subcarrier phase at this position
            double phase_ref = TAU * fsc * col / output_rate;
            double val = row[col];
            sum_sin += val * std::sin(phase_ref);
            sum_cos += val * std::cos(phase_ref);
            ++count;
        }
    }

    if (count == 0) return 0;

    double angle = std::atan2(sum_sin, sum_cos);
    if (angle < 0.0) angle += TAU;

    // Map to quadrant: 0 = [315..45), 1 = [45..135), 2 = [135..225), 3 = [225..315)
    int quadrant = static_cast<int>(std::floor((angle + PI / 4.0) / (PI / 2.0))) % 4;
    return quadrant;
}

// Measure burst cancellation metric for track detection.
// NTSC burst alternates 180° per line. With correct track phase,
// summing adjacent-line bursts should cancel (low metric).
// Wrong track → constructive addition → high metric.
// Matches cuVHS measure_burst_cancellation exactly.
static double measure_burst_cancellation(const double* chroma_lines,
                                         int output_field_lines, int line_len,
                                         int burst_start, int burst_end,
                                         double fft_scale)
{
    const int SKIP = 16;  // skip first/last 16 lines (vblank, head switch)
    int burst_len = burst_end - burst_start;
    double total = 0.0;
    int count = 0;

    for (int line = SKIP; line < output_field_lines - SKIP - 1; line += 2) {
        const double* line_a = chroma_lines + line * line_len;
        const double* line_b = chroma_lines + (line + 1) * line_len;

        double sum = 0.0;
        for (int col = burst_start; col < burst_end; ++col) {
            double a = line_a[col] * fft_scale;
            double b = line_b[col] * fft_scale;
            sum += std::fabs(a + b);
        }
        total += sum / burst_len;
        ++count;
    }
    return (count > 0) ? (total / count) : 1e9;
}

// ---------------------------------------------------------------------------
// Process one field with a given track assumption, writing chroma lines
// into a temporary double buffer for measurement purposes.
//
// This is the inner loop shared by track detection and final output.
// When output_u16 is non-null, also writes final uint16 output.
// ---------------------------------------------------------------------------

static void process_field_chroma(const double* filtered,       // bandpass-filtered raw
                                 const double* linelocs,
                                 int total_raw_samples,
                                 int track,
                                 int het_phase_offset,         // base phase offset (quadrants)
                                 double* chroma_lines,         // output doubles [field_lines * line_fft_size]
                                 uint16_t* output_u16,         // output uint16 [field_lines * output_line_len] or nullptr
                                 const VideoFormat& fmt,
                                 ChromaDemodState& ds)
{
    const int out_lines = fmt.output_field_lines;   // [NTSC-SPECIFIC] 263
    const int out_cols  = fmt.output_line_len;       // [NTSC-SPECIFIC] 910
    const int line_start = fmt.active_line_start;    // [NTSC-SPECIFIC] 10
    const int lpf = fmt.lines_per_frame;             // [NTSC-SPECIFIC] 525
    const double fsc = fmt.fsc;
    const double chroma_under = fmt.chroma_under;
    const double output_rate = fmt.output_rate;
    const double burst_abs_ref = fmt.burst_abs_ref;

    const double het_scale = (fsc + chroma_under) / output_rate;
    const double fft_scale = 1.0 / ds.line_fft_size;

    // [NTSC-SPECIFIC] Track-dependent rotation: Track 0 -> rot=3 (-90 deg/line),
    // Track 1 -> rot=1 (+90 deg/line)
    const int rot = (track == 0) ? 3 : 1;

    // Burst window in output samples
    const int burst_start = static_cast<int>(fmt.burst_start_us * 1e-6 * output_rate);
    const int burst_end   = static_cast<int>(fmt.burst_end_us * 1e-6 * output_rate);

    // Bandwidth for chroma bandpass in output FFT domain: fsc +/- 500 kHz
    const double bw = 500e3;
    const double fsc_bin_f = fsc * ds.line_fft_size / output_rate;
    const int bw_bins = static_cast<int>(std::ceil(bw * ds.line_fft_size / output_rate));
    const int fsc_bin_lo = std::max(0, static_cast<int>(fsc_bin_f) - bw_bins);
    const int fsc_bin_hi = std::min(ds.line_freq_bins - 1,
                                    static_cast<int>(fsc_bin_f) + bw_bins);

    for (int out_line = 0; out_line < out_lines; ++out_line) {
        int ll_line = out_line + line_start;
        int ll_next = ll_line + 1;

        double* chroma_row = chroma_lines + out_line * ds.line_fft_size;

        // Default: zero chroma
        std::memset(chroma_row, 0, ds.line_fft_size * sizeof(double));
        if (output_u16) {
            uint16_t* u16_row = output_u16 + out_line * out_cols;
            for (int c = 0; c < out_cols; ++c) u16_row[c] = 32768;
        }

        if (ll_next >= lpf) continue;

        double line_begin = linelocs[ll_line];
        double line_end   = linelocs[ll_next];

        // [NTSC-SPECIFIC] Compute heterodyne phase for this line
        // Continuous phase: absolute sample position, not per-line reset
        int line_phase = (out_line * rot + het_phase_offset) & 3;
        double phase_offset = line_phase * (PI / 2.0);

        // Step a: TBC-resample + heterodyne into line_buf
        double* lbuf = ds.line_buf;
        std::memset(lbuf, 0, ds.line_fft_size * sizeof(double));

        for (int out_col = 0; out_col < out_cols; ++out_col) {
            // Interpolate input position (Catmull-Rom, same as tbc_resample)
            double frac  = static_cast<double>(out_col) / static_cast<double>(out_cols);
            double coord = line_begin + frac * (line_end - line_begin);

            int ci = static_cast<int>(coord);
            double x = coord - ci;

            double sample_val = 0.0;
            if (ci >= 1 && ci + 2 < total_raw_samples) {
                double p0 = filtered[ci - 1];
                double p1 = filtered[ci];
                double p2 = filtered[ci + 1];
                double p3 = filtered[ci + 2];

                double a = p2 - p0;
                double b = 2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3;
                double c = 3.0 * (p1 - p2) + p3 - p0;
                sample_val = p1 + 0.5 * x * (a + x * (b + x * c));
            }

            // Heterodyne: multiply by -cos(2pi * het_scale * abs_sample + phase_offset)
            // abs_sample uses continuous position across the field
            double abs_sample = static_cast<double>(out_line * out_cols + out_col);
            double het = -std::cos(TAU * het_scale * abs_sample + phase_offset);
            lbuf[out_col] = sample_val * het;
        }

        // Step d: Forward FFT
        fftw_execute_dft_r2c(ds.line_plan_fwd, lbuf, ds.line_fft);

        // Step e: Bandpass -- zero bins outside fsc +/- bandwidth
        for (int k = 0; k < ds.line_freq_bins; ++k) {
            if (k < fsc_bin_lo || k > fsc_bin_hi) {
                ds.line_fft[k][0] = 0.0;
                ds.line_fft[k][1] = 0.0;
            }
        }

        // Step f: Inverse FFT
        fftw_execute_dft_c2r(ds.line_plan_inv, ds.line_fft, lbuf);

        // Copy result to chroma_lines for burst measurement
        std::memcpy(chroma_row, lbuf, ds.line_fft_size * sizeof(double));

        // Step g+h: Burst RMS, ACC scale, output
        if (output_u16) {
            // Measure burst RMS for this line
            double burst_energy = 0.0;
            int burst_count = 0;
            for (int col = burst_start; col < burst_end && col < out_cols; ++col) {
                double v = lbuf[col] * fft_scale;
                burst_energy += v * v;
                ++burst_count;
            }
            double rms = (burst_count > 0)
                ? std::sqrt(burst_energy / burst_count)
                : 1.0;
            double acc_scale = (rms > 1e-6) ? (burst_abs_ref / rms) : 1.0;

            // DEBUG: print burst info for first few lines of first field
            static int dbg_chroma_count = 0;
            if (dbg_chroma_count < 5 && out_line == 50) {
                fprintf(stderr, "    [CHROMA] line %d: burst_rms=%.1f acc_scale=%.3f burst_ref=%.0f\n",
                        out_line, rms, acc_scale, burst_abs_ref);
                dbg_chroma_count++;
            }

            uint16_t* u16_row = output_u16 + out_line * out_cols;
            for (int col = 0; col < out_cols; ++col) {
                double val = lbuf[col] * fft_scale * acc_scale + 32768.0;
                val = std::clamp(val, 0.0, 65535.0);
                u16_row[col] = static_cast<uint16_t>(std::lround(val));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// ChromaDemodState::init
// ---------------------------------------------------------------------------

bool ChromaDemodState::init(const VideoFormat& fmt)
{
    double fs = fmt.sample_rate;
    int spf = fmt.samples_per_field;

    // -------------------------------------------------------------------
    // Step 1: Build pre-bandpass filter for color-under band
    // [NTSC-SPECIFIC] BPF 60kHz - 1.2MHz, order 4, squared magnitude
    // (sosfiltfilt equivalent: forward+backward = squared magnitude)
    // -------------------------------------------------------------------
    raw_fft_size  = next_fft_size(spf);
    raw_freq_bins = raw_fft_size / 2 + 1;

    double inv_N = 1.0 / raw_fft_size;

    bpf_filter = (double*)fftw_malloc(raw_freq_bins * sizeof(double));
    bpf_padded = (double*)fftw_malloc(raw_fft_size * sizeof(double));
    bpf_fft    = (fftw_complex*)fftw_malloc(raw_freq_bins * sizeof(fftw_complex));

    if (!bpf_filter || !bpf_padded || !bpf_fft) return false;

    for (int k = 0; k < raw_freq_bins; ++k) {
        double f = static_cast<double>(k) * fs / raw_fft_size;
        // [NTSC-SPECIFIC] Color-under band: 60kHz to 1.2MHz, order 4
        double mag = butter_bpf_mag(f, 60e3, 1.2e6, 4);
        // Squared magnitude for sosfiltfilt equivalent (forward+backward)
        // Also include 1/N for FFTW normalization
        bpf_filter[k] = mag * mag * inv_N;
    }

    // -------------------------------------------------------------------
    // Step 2: Create FFTW plans for capture-rate bandpass
    // -------------------------------------------------------------------
    bpf_plan_fwd = fftw_plan_dft_r2c_1d(raw_fft_size, bpf_padded, bpf_fft, FFTW_ESTIMATE);
    bpf_plan_inv = fftw_plan_dft_c2r_1d(raw_fft_size, bpf_fft, bpf_padded, FFTW_ESTIMATE);

    if (!bpf_plan_fwd || !bpf_plan_inv) return false;

    // -------------------------------------------------------------------
    // Step 3: Compute line FFT size (7-smooth, better for FFTW than pow2)
    // -------------------------------------------------------------------
    line_fft_size  = next_fft_size(fmt.output_line_len);
    line_freq_bins = line_fft_size / 2 + 1;

    // -------------------------------------------------------------------
    // Step 4: Create FFTW plans for per-line processing
    // -------------------------------------------------------------------
    line_buf = (double*)fftw_malloc(line_fft_size * sizeof(double));
    line_fft = (fftw_complex*)fftw_malloc(line_freq_bins * sizeof(fftw_complex));

    if (!line_buf || !line_fft) return false;

    line_plan_fwd = fftw_plan_dft_r2c_1d(line_fft_size, line_buf, line_fft, FFTW_ESTIMATE);
    line_plan_inv = fftw_plan_dft_c2r_1d(line_fft_size, line_fft, line_buf, FFTW_ESTIMATE);

    if (!line_plan_fwd || !line_plan_inv) return false;

    return true;
}

// ---------------------------------------------------------------------------
// ChromaDemodState::destroy
// ---------------------------------------------------------------------------

void ChromaDemodState::destroy()
{
    if (bpf_plan_fwd)   { fftw_destroy_plan(bpf_plan_fwd);   bpf_plan_fwd   = nullptr; }
    if (bpf_plan_inv)   { fftw_destroy_plan(bpf_plan_inv);   bpf_plan_inv   = nullptr; }
    if (line_plan_fwd)  { fftw_destroy_plan(line_plan_fwd);  line_plan_fwd  = nullptr; }
    if (line_plan_inv)  { fftw_destroy_plan(line_plan_inv);  line_plan_inv  = nullptr; }

    if (bpf_filter)     { fftw_free(bpf_filter);  bpf_filter  = nullptr; }
    if (bpf_padded)     { fftw_free(bpf_padded);  bpf_padded  = nullptr; }
    if (bpf_fft)        { fftw_free(bpf_fft);     bpf_fft     = nullptr; }
    if (line_buf)       { fftw_free(line_buf);     line_buf    = nullptr; }
    if (line_fft)       { fftw_free(line_fft);     line_fft    = nullptr; }

    raw_fft_size  = 0;
    raw_freq_bins = 0;
    line_fft_size  = 0;
    line_freq_bins = 0;
}

// ---------------------------------------------------------------------------
// chroma_decode -- process ONE field
// ---------------------------------------------------------------------------

void chroma_decode(const double* raw,
                   const double* linelocs,
                   double* scratch,
                   uint16_t* tbc_chroma,
                   int is_first_field,
                   int samples_per_field,
                   int total_raw_samples,
                   int* field_phase_id,
                   const VideoFormat& fmt,
                   ChromaState& chroma_state,
                   ChromaDemodState& demod_state)
{
    const int out_lines = fmt.output_field_lines;
    const int out_cols  = fmt.output_line_len;

    // Burst window in output samples
    const int burst_start = static_cast<int>(fmt.burst_start_us * 1e-6 * fmt.output_rate);
    const int burst_end   = static_cast<int>(fmt.burst_end_us * 1e-6 * fmt.output_rate);

    // -------------------------------------------------------------------
    // Step 0: Pre-bandpass filter raw signal -> scratch
    // Isolate color-under band (60kHz - 1.2MHz) before heterodyne.
    // Without this, luma FM carrier contaminates chroma causing cycling
    // color artifacts.
    //
    // Process in spf-sized chunks (FFT plan is sized for spf) to cover
    // the full total_raw_samples buffer. Lines past spf need valid data.
    // -------------------------------------------------------------------
    {
        int N = demod_state.raw_fft_size;
        int remaining = total_raw_samples;
        int offset = 0;

        while (remaining > 0) {
            int chunk = std::min(remaining, samples_per_field);

            std::memcpy(demod_state.bpf_padded, raw + offset,
                        chunk * sizeof(double));
            std::memset(demod_state.bpf_padded + chunk, 0,
                        (N - chunk) * sizeof(double));

            fftw_execute_dft_r2c(demod_state.bpf_plan_fwd,
                                 demod_state.bpf_padded,
                                 demod_state.bpf_fft);

            for (int k = 0; k < demod_state.raw_freq_bins; ++k) {
                demod_state.bpf_fft[k][0] *= demod_state.bpf_filter[k];
                demod_state.bpf_fft[k][1] *= demod_state.bpf_filter[k];
            }

            fftw_execute_dft_c2r(demod_state.bpf_plan_inv,
                                 demod_state.bpf_fft,
                                 demod_state.bpf_padded);

            std::memcpy(scratch + offset, demod_state.bpf_padded,
                        chunk * sizeof(double));

            offset += chunk;
            remaining -= chunk;
        }
    }

    // Temporary chroma lines buffer for measurement (doubles)
    int chroma_buf_size = out_lines * demod_state.line_fft_size;
    std::vector<double> chroma_lines(chroma_buf_size, 0.0);

    // -------------------------------------------------------------------
    // Step 1: Track detection (only if !chroma_state.valid)
    //
    // Process this field with track=0 and track=1, measure burst
    // cancellation for each. The correct track produces lower residual
    // burst energy after heterodyne.
    // -------------------------------------------------------------------
    static int chroma_dbg_field = 0;
    if (!chroma_state.valid) {
        std::vector<double> chroma_t0(chroma_buf_size, 0.0);
        std::vector<double> chroma_t1(chroma_buf_size, 0.0);

        // Try track 0 with default phase offset 0
        process_field_chroma(scratch, linelocs, total_raw_samples,
                             0, 0, chroma_t0.data(), nullptr,
                             fmt, demod_state);

        // Try track 1 with default phase offset 0
        process_field_chroma(scratch, linelocs, total_raw_samples,
                             1, 0, chroma_t1.data(), nullptr,
                             fmt, demod_state);

        double metric0 = measure_burst_cancellation(
            chroma_t0.data(), out_lines, demod_state.line_fft_size,
            burst_start, burst_end, 1.0 / demod_state.line_fft_size);
        double metric1 = measure_burst_cancellation(
            chroma_t1.data(), out_lines, demod_state.line_fft_size,
            burst_start, burst_end, 1.0 / demod_state.line_fft_size);

        if (metric0 <= metric1) {
            chroma_state.current_track = 0;
            chroma_state.good_metric_threshold = metric0 * 4.0;
        } else {
            chroma_state.current_track = 1;
            chroma_state.good_metric_threshold = metric1 * 4.0;
        }

        // Measure burst phase from the winning track's output
        const double* winning = (chroma_state.current_track == 0)
            ? chroma_t0.data() : chroma_t1.data();
        int quadrant = measure_burst_phase(
            winning, out_lines, demod_state.line_fft_size,
            burst_start, burst_end, fmt.fsc, fmt.output_rate);

        // [NTSC-SPECIFIC] Look up phase from table
        int delta = 0;
        if (chroma_state.prev_quadrant >= 0) {
            delta = quadrant - chroma_state.prev_quadrant;
        }
        NTSCPhaseResult phase_result = lookup_ntsc_phase(
            is_first_field, quadrant, delta);

        chroma_state.cycle_start = phase_result.offset;
        chroma_state.prev_quadrant = quadrant;
        chroma_state.valid = true;
    }

    // -------------------------------------------------------------------
    // Step 2: Use carried track/phase state
    //
    // het_offset is ALWAYS 0 — cuVHS sets field_phase_offset=0 for all
    // fields. The cycle_start is only used for field_phase_id metadata,
    // NOT for the heterodyne phase.
    // -------------------------------------------------------------------
    int track = chroma_state.current_track;
    int het_offset = 0;

    // -------------------------------------------------------------------
    // Step 3: Process field with final output
    // -------------------------------------------------------------------
    process_field_chroma(scratch, linelocs, total_raw_samples,
                         track, het_offset,
                         chroma_lines.data(), tbc_chroma,
                         fmt, demod_state);

    // -------------------------------------------------------------------
    // Step 4: Track flip detection
    //
    // After processing, check if burst cancellation is poor. If so, the
    // track assumption is wrong (tape head switch, edit, etc). Flip track
    // and re-process. Limit retries to avoid infinite loop.
    // -------------------------------------------------------------------
    {
        double fft_scale = 1.0 / demod_state.line_fft_size;
        double metric = measure_burst_cancellation(
            chroma_lines.data(), out_lines, demod_state.line_fft_size,
            burst_start, burst_end, fft_scale);

        int retries = 0;
        const int max_retries = 2;

        while (metric > chroma_state.good_metric_threshold && retries < max_retries) {
            // Flip track
            track ^= 1;

            // Re-process with flipped track
            process_field_chroma(scratch, linelocs, total_raw_samples,
                                 track, het_offset,
                                 chroma_lines.data(), tbc_chroma,
                                 fmt, demod_state);

            metric = measure_burst_cancellation(
                chroma_lines.data(), out_lines, demod_state.line_fft_size,
                burst_start, burst_end, fft_scale);

            // Update threshold from this better metric if improved
            if (metric < chroma_state.good_metric_threshold / 4.0) {
                chroma_state.good_metric_threshold = metric * 4.0;
            }

            ++retries;
        }

        chroma_state.current_track = track;

        if (chroma_dbg_field < 10) {
            fprintf(stderr, "    [CHROMA DBG] field %d: track=%d metric=%.1f threshold=%.1f retries=%d\n",
                    chroma_dbg_field, track, metric, chroma_state.good_metric_threshold, retries);
        }
    }

    // -------------------------------------------------------------------
    // Step 5: Update chroma_state for next field
    // -------------------------------------------------------------------
    {
        // Measure burst phase of final output
        int quadrant = measure_burst_phase(
            chroma_lines.data(), out_lines, demod_state.line_fft_size,
            burst_start, burst_end, fmt.fsc, fmt.output_rate);

        chroma_state.prev_quadrant = quadrant;

        // Toggle track for next field (VHS alternates track per field)
        chroma_state.current_track ^= 1;

        // Advance cycle position within the 8-field phase cycle
        chroma_state.cycle_start = (chroma_state.cycle_start + 1) & 3;
    }

    // -------------------------------------------------------------------
    // Step 6: Set field_phase_id from NTSC cycle
    // -------------------------------------------------------------------
    {
        int quadrant = chroma_state.prev_quadrant;
        int delta = 0;  // delta not meaningful for single lookup
        NTSCPhaseResult phase_result = lookup_ntsc_phase(
            is_first_field, quadrant, delta);
        *field_phase_id = phase_result.phase_id;
    }
    chroma_dbg_field++;
}

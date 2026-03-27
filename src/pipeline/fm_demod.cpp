#include "pipeline/fm_demod.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <algorithm>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static const double PI  = 3.14159265358979323846;
static const double TAU = 2.0 * PI;

// ---------------------------------------------------------------------------
// FFT size helpers -- find next 7-smooth number for efficient FFTs
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
// ---------------------------------------------------------------------------

// Butterworth LPF magnitude at frequency f, cutoff fc, order N
static double butter_lpf_mag(double f, double fc, int order)
{
    double ratio = f / fc;
    double r2n = 1.0;
    for (int i = 0; i < order; ++i) r2n *= ratio * ratio;
    return 1.0 / std::sqrt(1.0 + r2n);
}

// Butterworth HPF magnitude at frequency f, cutoff fc, order N
static double butter_hpf_mag(double f, double fc, int order)
{
    if (f == 0.0) return 0.0;
    double ratio = fc / f;
    double r2n = 1.0;
    for (int i = 0; i < order; ++i) r2n *= ratio * ratio;
    return 1.0 / std::sqrt(1.0 + r2n);
}

// Butterworth BPF magnitude (cascade of HPF + LPF)
static double butter_bpf_mag(double f, double f_low, double f_high, int order)
{
    return butter_hpf_mag(f, f_low, order) * butter_lpf_mag(f, f_high, order);
}

// ---------------------------------------------------------------------------
// Super-Gaussian lowpass
// ---------------------------------------------------------------------------

static double supergauss_mag(double f, double fc, int order)
{
    double ratio = f / fc;
    double r2n = 1.0;
    for (int i = 0; i < order; ++i) r2n *= ratio * ratio;
    return std::exp(-0.5 * r2n);
}

// ---------------------------------------------------------------------------
// Shelf filter (deemphasis) -- biquad frequency response
// ---------------------------------------------------------------------------

// Generate high-shelf biquad coefficients.
// Returns {b0, b1, b2, a0, a1, a2}.
static void gen_shelf_high(double fc, double gain_db, double Q,
                           double sample_rate,
                           double b[3], double a[3])
{
    double A  = std::pow(10.0, gain_db / 40.0);  // amplitude from dB
    double w0 = TAU * fc / sample_rate;
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double alpha  = sinw0 / (2.0 * Q);
    double sqrtA  = std::sqrt(A);

    b[0] =      A * ((A + 1.0) + (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha);
    b[1] = -2.0*A * ((A - 1.0) + (A + 1.0) * cosw0);
    b[2] =      A * ((A + 1.0) + (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha);
    a[0] =           (A + 1.0) - (A - 1.0) * cosw0 + 2.0 * sqrtA * alpha;
    a[1] =  2.0   * ((A - 1.0) - (A + 1.0) * cosw0);
    a[2] =           (A + 1.0) - (A - 1.0) * cosw0 - 2.0 * sqrtA * alpha;
}

// Evaluate biquad H(z) at normalized frequency f/fs.
// Returns complex {re, im}.
static void freqz_biquad(const double b[3], const double a[3],
                          double f, double fs,
                          double& out_re, double& out_im)
{
    double w = TAU * f / fs;
    // Numerator: b0 + b1*z^-1 + b2*z^-2
    double num_re = b[0] + b[1] * std::cos(-w) + b[2] * std::cos(-2.0 * w);
    double num_im =        b[1] * std::sin(-w) + b[2] * std::sin(-2.0 * w);
    // Denominator: a0 + a1*z^-1 + a2*z^-2
    double den_re = a[0] + a[1] * std::cos(-w) + a[2] * std::cos(-2.0 * w);
    double den_im =        a[1] * std::sin(-w) + a[2] * std::sin(-2.0 * w);
    // Complex division: num / den
    double denom = den_re * den_re + den_im * den_im;
    if (denom < 1e-30) denom = 1e-30;
    out_re = (num_re * den_re + num_im * den_im) / denom;
    out_im = (num_im * den_re - num_re * den_im) / denom;
}

// ---------------------------------------------------------------------------
// FIR lowpass (Hamming window)
// ---------------------------------------------------------------------------

static void firwin_lpf(double* h, int ntaps, double cutoff, double fs)
{
    // cutoff is in Hz, fs is sample rate
    double fc_norm = cutoff / fs;  // normalized cutoff [0, 0.5]
    int M = ntaps - 1;
    double sum = 0.0;
    for (int i = 0; i < ntaps; ++i) {
        double n = i - M / 2.0;
        // Sinc
        double sinc;
        if (std::fabs(n) < 1e-12)
            sinc = 2.0 * fc_norm;
        else
            sinc = std::sin(TAU * fc_norm * n) / (PI * n);
        // Hamming window
        double w = 0.54 - 0.46 * std::cos(TAU * i / M);
        h[i] = sinc * w;
        sum += h[i];
    }
    // Normalize to unit DC gain (matches scipy.signal.firwin / cuVHS)
    for (int i = 0; i < ntaps; ++i) h[i] /= sum;
}

// Evaluate FIR frequency response at frequency f.
// Returns complex {re, im}.
static void fir_freqz(const double* h, int ntaps, double f, double fs,
                       int delay_compensate,
                       double& out_re, double& out_im)
{
    double w = TAU * f / fs;
    out_re = 0.0;
    out_im = 0.0;
    for (int i = 0; i < ntaps; ++i) {
        out_re += h[i] * std::cos(-w * i);
        out_im += h[i] * std::sin(-w * i);
    }
    // Group delay compensation: multiply by e^{j*w*delay}
    if (delay_compensate > 0) {
        double phase = w * delay_compensate;
        double cr = std::cos(phase);
        double ci = std::sin(phase);
        double re = out_re * cr - out_im * ci;
        double im = out_re * ci + out_im * cr;
        out_re = re;
        out_im = im;
    }
}

// ---------------------------------------------------------------------------
// FMDemodState::init -- create plans, allocate buffers, build filters
// ---------------------------------------------------------------------------

bool FMDemodState::init(const VideoFormat& fmt, int target_samples)
{
    double fs = fmt.sample_rate;
    spf = fmt.samples_per_field;

    // If target_samples specified, size FFT for that (e.g. 2*spf for
    // overlapping window). Otherwise default to spf.
    int n = (target_samples > 0) ? target_samples : spf;
    fft_size  = next_fft_size(n);
    freq_bins = fft_size / 2 + 1;

    // -------------------------------------------------------------------
    // Allocate scratch buffers
    // -------------------------------------------------------------------
    padded   = (double*)fftw_malloc(fft_size * sizeof(double));
    fft_half = (fftw_complex*)fftw_malloc(freq_bins * sizeof(fftw_complex));
    analytic = (fftw_complex*)fftw_malloc(fft_size * sizeof(fftw_complex));
    post_fft = (fftw_complex*)fftw_malloc(freq_bins * sizeof(fftw_complex));
    angles   = (double*)fftw_malloc(fft_size * sizeof(double));

    if (!padded || !fft_half || !analytic || !post_fft || !angles)
        return false;

    // -------------------------------------------------------------------
    // Create FFTW plans (NOT thread-safe -- each worker must call init
    // serially or under a mutex)
    // -------------------------------------------------------------------
    plan_r2c     = fftw_plan_dft_r2c_1d(fft_size, padded, fft_half, FFTW_ESTIMATE);
    plan_c2c_inv = fftw_plan_dft_1d(fft_size, analytic, analytic, FFTW_BACKWARD, FFTW_ESTIMATE);
    plan_c2r     = fftw_plan_dft_c2r_1d(fft_size, post_fft, padded, FFTW_ESTIMATE);

    if (!plan_r2c || !plan_c2c_inv || !plan_c2r)
        return false;

    // -------------------------------------------------------------------
    // Build RF bandpass filter (real-valued, applied to R2C half-spectrum)
    //
    // [NTSC-SPECIFIC] RF bandpass: BPF 500kHz-6.5MHz order 8,
    //                  LPF 6MHz order 25, HPF 1.2MHz order 20,
    //                  Hilbert weight (1.0 at DC/Nyquist, 2.0 elsewhere)
    // -------------------------------------------------------------------
    rf_filter = (double*)fftw_malloc(freq_bins * sizeof(double));
    if (!rf_filter) return false;

    for (int k = 0; k < freq_bins; ++k) {
        double f = (double)k * fs / fft_size;

        // [NTSC-SPECIFIC] Band edges and orders
        double bpf = butter_bpf_mag(f, 500e3, 6.5e6, 8);
        double lpf = butter_lpf_mag(f, 6e6, 25);
        double hpf = butter_hpf_mag(f, 1.2e6, 20);

        // Hilbert weight for analytic signal construction
        double hilbert = (k == 0 || k == freq_bins - 1) ? 1.0 : 2.0;

        rf_filter[k] = bpf * lpf * hpf * hilbert;
    }

    // -------------------------------------------------------------------
    // Build deemphasis shelf filter
    //
    // [NTSC-SPECIFIC] shelf at 273755.82 Hz, gain 13.9794 dB,
    //                  Q = 0.462088186
    //                  Inverted: swap a<->b in freqz evaluation
    // -------------------------------------------------------------------
    double shelf_b[3], shelf_a[3];
    gen_shelf_high(273755.82, 13.9794, 0.462088186, fs, shelf_b, shelf_a);

    // -------------------------------------------------------------------
    // Build sync FIR (65-tap Hamming-windowed LPF at 0.5 MHz)
    // -------------------------------------------------------------------
    const int fir_taps = 65;
    double fir_h[65];
    firwin_lpf(fir_h, fir_taps, 0.5e6, fs);
    f05_offset = 32;  // group delay = (ntaps-1)/2

    // -------------------------------------------------------------------
    // Build FVideo and FVideo05 (complex filters, length = freq_bins)
    //
    // FVideo   = deemph * video_lpf * (1/N)
    // FVideo05 = deemph * video_lpf * sync_fir * (1/N)
    //
    // Deemphasis is INVERTED: evaluate freqz with a,b swapped (a as
    // numerator, b as denominator) to get the inverse shelf response.
    // -------------------------------------------------------------------
    fvideo   = (fftw_complex*)fftw_malloc(freq_bins * sizeof(fftw_complex));
    fvideo05 = (fftw_complex*)fftw_malloc(freq_bins * sizeof(fftw_complex));
    if (!fvideo || !fvideo05) return false;

    double inv_N = 1.0 / (double)fft_size;

    for (int k = 0; k < freq_bins; ++k) {
        double f = (double)k * fs / fft_size;

        // Deemphasis: inverted shelf (swap a<->b: a is numerator, b is denominator)
        double de_re, de_im;
        freqz_biquad(shelf_a, shelf_b, f, fs, de_re, de_im);

        // [NTSC-SPECIFIC] Video LPF: super-Gaussian 6.6 MHz, order 9
        double vlpf = supergauss_mag(f, 6.6e6, 9);

        // Sync FIR frequency response (with group delay compensation)
        double fir_re, fir_im;
        fir_freqz(fir_h, fir_taps, f, fs, f05_offset, fir_re, fir_im);

        // FVideo = deemph * video_lpf * (1/N)
        fvideo[k][0] = de_re * vlpf * inv_N;
        fvideo[k][1] = de_im * vlpf * inv_N;

        // FVideo05 = deemph * video_lpf * sync_fir * (1/N)
        // Complex multiply: (de_re + j*de_im) * (fir_re + j*fir_im) * vlpf * inv_N
        double prod_re = de_re * fir_re - de_im * fir_im;
        double prod_im = de_re * fir_im + de_im * fir_re;
        fvideo05[k][0] = prod_re * vlpf * inv_N;
        fvideo05[k][1] = prod_im * vlpf * inv_N;
    }

    return true;
}

// ---------------------------------------------------------------------------
// FMDemodState::destroy
// ---------------------------------------------------------------------------

void FMDemodState::destroy()
{
    if (plan_r2c)     { fftw_destroy_plan(plan_r2c);     plan_r2c     = nullptr; }
    if (plan_c2c_inv) { fftw_destroy_plan(plan_c2c_inv); plan_c2c_inv = nullptr; }
    if (plan_c2r)     { fftw_destroy_plan(plan_c2r);     plan_c2r     = nullptr; }

    if (rf_filter) { fftw_free(rf_filter); rf_filter = nullptr; }
    if (fvideo)    { fftw_free(fvideo);    fvideo    = nullptr; }
    if (fvideo05)  { fftw_free(fvideo05);  fvideo05  = nullptr; }
    if (padded)    { fftw_free(padded);    padded    = nullptr; }
    if (fft_half)  { fftw_free(fft_half);  fft_half  = nullptr; }
    if (analytic)  { fftw_free(analytic);  analytic  = nullptr; }
    if (post_fft)  { fftw_free(post_fft);  post_fft  = nullptr; }
    if (angles)    { fftw_free(angles);    angles    = nullptr; }

    fft_size  = 0;
    freq_bins = 0;
    spf       = 0;
}

// ---------------------------------------------------------------------------
// fm_demod -- process ONE field
// ---------------------------------------------------------------------------

void fm_demod(FMDemodState& state,
              const double* raw,
              double* demod,
              double* demod05,
              double* envelope,
              int samples_per_field,
              const VideoFormat& fmt)
{
    const int N    = state.fft_size;
    const int bins = state.freq_bins;
    const double fs = fmt.sample_rate;

    double*       padded   = state.padded;
    fftw_complex* fft_half = state.fft_half;
    fftw_complex* ana      = state.analytic;
    fftw_complex* post     = state.post_fft;
    double*       ang      = state.angles;

    // -------------------------------------------------------------------
    // Step 1: Zero-pad raw to fft_size
    // -------------------------------------------------------------------
    std::memcpy(padded, raw, samples_per_field * sizeof(double));
    std::memset(padded + samples_per_field, 0,
                (N - samples_per_field) * sizeof(double));

    // -------------------------------------------------------------------
    // Step 2: R2C FFT
    // -------------------------------------------------------------------
    fftw_execute_dft_r2c(state.plan_r2c, padded, fft_half);

    // -------------------------------------------------------------------
    // Step 3: Apply RF filter (real multiply, includes Hilbert weight)
    // -------------------------------------------------------------------
    for (int k = 0; k < bins; ++k) {
        fft_half[k][0] *= state.rf_filter[k];
        fft_half[k][1] *= state.rf_filter[k];
    }

    // -------------------------------------------------------------------
    // Step 4: Expand half-spectrum to full analytic spectrum
    //         Copy fft_half into analytic[0..freq_bins-1], zero the rest.
    //         Hilbert weighting (x2 for positive freqs) is already baked
    //         into rf_filter, so this is a straight copy.
    // -------------------------------------------------------------------
    for (int k = 0; k < bins; ++k) {
        ana[k][0] = fft_half[k][0];
        ana[k][1] = fft_half[k][1];
    }
    for (int k = bins; k < N; ++k) {
        ana[k][0] = 0.0;
        ana[k][1] = 0.0;
    }

    // -------------------------------------------------------------------
    // Step 5: C2C inverse FFT -> analytic signal (time domain)
    // -------------------------------------------------------------------
    fftw_execute_dft(state.plan_c2c_inv, ana, ana);

    // -------------------------------------------------------------------
    // Step 5b: Compute envelope: sqrt(re^2 + im^2) for each sample
    // -------------------------------------------------------------------
    if (envelope) {
        for (int i = 0; i < samples_per_field; ++i) {
            double re = ana[i][0];
            double im = ana[i][1];
            envelope[i] = std::sqrt(re * re + im * im);
        }
    }

    // -------------------------------------------------------------------
    // Step 6a: Extract phase angles
    // -------------------------------------------------------------------
    for (int i = 0; i < N; ++i) {
        ang[i] = std::atan2(ana[i][1], ana[i][0]);
    }

    // -------------------------------------------------------------------
    // Step 6b: Phase unwrap -> instantaneous frequency in Hz
    //          Sequential per field -- MUST match cuVHS algorithm exactly.
    // -------------------------------------------------------------------
    demod[0] = 0.0;
    double prev_raw_dangle = 0.0;
    double correction = 0.0;
    int spf_clamped = std::min(N, samples_per_field);
    for (int i = 1; i < spf_clamped; ++i) {
        double dangle = ang[i] - ang[i - 1];
        double delta  = dangle - prev_raw_dangle;
        prev_raw_dangle = dangle;
        if (delta > PI)        correction -= TAU;
        else if (delta < -PI)  correction += TAU;
        double unwrapped = dangle + correction;
        double clamped = std::fmod(unwrapped, TAU);
        if (clamped < 0.0) clamped += TAU;
        demod[i] = clamped * (fs / TAU);
    }

    // -------------------------------------------------------------------
    // Step 7: Copy demod into padded, zero-pad remainder
    // -------------------------------------------------------------------
    std::memcpy(padded, demod, samples_per_field * sizeof(double));
    std::memset(padded + samples_per_field, 0,
                (N - samples_per_field) * sizeof(double));

    // -------------------------------------------------------------------
    // Step 8: R2C FFT of demod
    // -------------------------------------------------------------------
    fftw_execute_dft_r2c(state.plan_r2c, padded, fft_half);

    // -------------------------------------------------------------------
    // Step 9: Apply FVideo filter -> video output
    //         Complex multiply fft_half * fvideo -> post_fft, then C2R.
    //         NOTE: C2R destroys post_fft (its input), but fft_half
    //         remains valid for step 10.
    // -------------------------------------------------------------------
    for (int k = 0; k < bins; ++k) {
        double hr = fft_half[k][0];
        double hi = fft_half[k][1];
        double fr = state.fvideo[k][0];
        double fi = state.fvideo[k][1];
        post[k][0] = hr * fr - hi * fi;
        post[k][1] = hr * fi + hi * fr;
    }
    fftw_execute_dft_c2r(state.plan_c2r, post, padded);

    // Trim to samples_per_field
    std::memcpy(demod, padded, samples_per_field * sizeof(double));

    // -------------------------------------------------------------------
    // Step 10: Apply FVideo05 filter -> sync output
    //          fft_half is still valid from step 8 (C2R only destroys
    //          post_fft, not fft_half). No need to re-FFT.
    // -------------------------------------------------------------------
    for (int k = 0; k < bins; ++k) {
        double hr = fft_half[k][0];
        double hi = fft_half[k][1];
        double fr = state.fvideo05[k][0];
        double fi = state.fvideo05[k][1];
        post[k][0] = hr * fr - hi * fi;
        post[k][1] = hr * fi + hi * fr;
    }
    fftw_execute_dft_c2r(state.plan_c2r, post, padded);

    // Trim to samples_per_field
    std::memcpy(demod05, padded, samples_per_field * sizeof(double));
}

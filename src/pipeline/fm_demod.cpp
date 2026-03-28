#include "pipeline/fm_demod.h"
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

static void gen_shelf_high(double fc, double gain_db, double Q,
                           double sample_rate,
                           double b[3], double a[3])
{
    double A  = std::pow(10.0, gain_db / 40.0);
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

static void freqz_biquad(const double b[3], const double a[3],
                          double f, double fs,
                          double& out_re, double& out_im)
{
    double w = TAU * f / fs;
    double num_re = b[0] + b[1] * std::cos(-w) + b[2] * std::cos(-2.0 * w);
    double num_im =        b[1] * std::sin(-w) + b[2] * std::sin(-2.0 * w);
    double den_re = a[0] + a[1] * std::cos(-w) + a[2] * std::cos(-2.0 * w);
    double den_im =        a[1] * std::sin(-w) + a[2] * std::sin(-2.0 * w);
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
    double fc_norm = cutoff / fs;
    int M = ntaps - 1;
    double sum = 0.0;
    for (int i = 0; i < ntaps; ++i) {
        double n = i - M / 2.0;
        double sinc;
        if (std::fabs(n) < 1e-12)
            sinc = 2.0 * fc_norm;
        else
            sinc = std::sin(TAU * fc_norm * n) / (PI * n);
        double w = 0.54 - 0.46 * std::cos(TAU * i / M);
        h[i] = sinc * w;
        sum += h[i];
    }
    for (int i = 0; i < ntaps; ++i) h[i] /= sum;
}

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
// Build all frequency-domain filters for a given fft_size
// ---------------------------------------------------------------------------

static void build_filters(int fft_size, int freq_bins, double fs,
                          double* rf_filter,
                          fftw_complex* fvideo, fftw_complex* fvideo05,
                          int& f05_offset)
{
    // [NTSC-SPECIFIC] RF bandpass: BPF 500kHz-6.5MHz order 8,
    //                  LPF 6MHz order 25, HPF 1.2MHz order 20,
    //                  Hilbert weight (1.0 at DC/Nyquist, 2.0 elsewhere)
    for (int k = 0; k < freq_bins; ++k) {
        double f = (double)k * fs / fft_size;
        double bpf = butter_bpf_mag(f, 500e3, 6.5e6, 8);
        double lpf = butter_lpf_mag(f, 6e6, 25);
        double hpf = butter_hpf_mag(f, 1.2e6, 20);
        double hilbert = (k == 0 || k == freq_bins - 1) ? 1.0 : 2.0;
        rf_filter[k] = bpf * lpf * hpf * hilbert;
    }

    // [NTSC-SPECIFIC] shelf at 273755.82 Hz, gain 13.9794 dB, Q = 0.462088186
    // Inverted: swap a<->b in freqz evaluation
    double shelf_b[3], shelf_a[3];
    gen_shelf_high(273755.82, 13.9794, 0.462088186, fs, shelf_b, shelf_a);

    const int fir_taps = 65;
    double fir_h[65];
    firwin_lpf(fir_h, fir_taps, 0.5e6, fs);
    f05_offset = 32;

    double inv_N = 1.0 / (double)fft_size;
    for (int k = 0; k < freq_bins; ++k) {
        double f = (double)k * fs / fft_size;

        // Deemphasis: inverted shelf (swap a<->b)
        double de_re, de_im;
        freqz_biquad(shelf_a, shelf_b, f, fs, de_re, de_im);

        // [NTSC-SPECIFIC] Video LPF: super-Gaussian 6.6 MHz, order 9
        double vlpf = supergauss_mag(f, 6.6e6, 9);

        double fir_re, fir_im;
        fir_freqz(fir_h, fir_taps, f, fs, f05_offset, fir_re, fir_im);

        fvideo[k][0] = de_re * vlpf * inv_N;
        fvideo[k][1] = de_im * vlpf * inv_N;

        double prod_re = de_re * fir_re - de_im * fir_im;
        double prod_im = de_re * fir_im + de_im * fir_re;
        fvideo05[k][0] = prod_re * vlpf * inv_N;
        fvideo05[k][1] = prod_im * vlpf * inv_N;
    }
}

// ---------------------------------------------------------------------------
// FMDemodState::init -- create plans, allocate buffers, build filters
// ---------------------------------------------------------------------------

bool FMDemodState::init(const VideoFormat& fmt, int target_samples, int tile_sz)
{
    (void)tile_sz;  // tiling removed — always full-field
    double fs = fmt.sample_rate;
    spf = fmt.samples_per_field;

    int n = (target_samples > 0) ? target_samples : spf;
    fft_size = next_fft_size(n);
    tile_overlap = 0;
    tile_step = 0;

    freq_bins = fft_size / 2 + 1;

    padded   = (double*)fftw_malloc(fft_size * sizeof(double));
    fft_half = (fftw_complex*)fftw_malloc(freq_bins * sizeof(fftw_complex));
    analytic = (fftw_complex*)fftw_malloc(fft_size * sizeof(fftw_complex));
    post_fft = (fftw_complex*)fftw_malloc(freq_bins * sizeof(fftw_complex));
    angles   = (double*)fftw_malloc(fft_size * sizeof(double));

    if (!padded || !fft_half || !analytic || !post_fft || !angles)
        return false;

    plan_r2c     = fftw_plan_dft_r2c_1d(fft_size, padded, fft_half, FFTW_ESTIMATE);
    plan_c2c_inv = fftw_plan_dft_1d(fft_size, analytic, analytic, FFTW_BACKWARD, FFTW_ESTIMATE);
    plan_c2r     = fftw_plan_dft_c2r_1d(fft_size, post_fft, padded, FFTW_ESTIMATE);

    if (!plan_r2c || !plan_c2c_inv || !plan_c2r)
        return false;

    rf_filter = (double*)fftw_malloc(freq_bins * sizeof(double));
    fvideo    = (fftw_complex*)fftw_malloc(freq_bins * sizeof(fftw_complex));
    fvideo05  = (fftw_complex*)fftw_malloc(freq_bins * sizeof(fftw_complex));
    if (!rf_filter || !fvideo || !fvideo05) return false;

    build_filters(fft_size, freq_bins, fs, rf_filter, fvideo, fvideo05, f05_offset);

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
    tile_overlap = 0;
    tile_step = 0;
}

// ---------------------------------------------------------------------------
// fm_demod_fullfield -- single-FFT path (tile_step == 0)
// ---------------------------------------------------------------------------

static void fm_demod_fullfield(FMDemodState& state,
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

    std::memcpy(padded, raw, samples_per_field * sizeof(double));
    std::memset(padded + samples_per_field, 0, (N - samples_per_field) * sizeof(double));

    fftw_execute_dft_r2c(state.plan_r2c, padded, fft_half);

    for (int k = 0; k < bins; ++k) {
        fft_half[k][0] *= state.rf_filter[k];
        fft_half[k][1] *= state.rf_filter[k];
    }

    for (int k = 0; k < bins; ++k) {
        ana[k][0] = fft_half[k][0];
        ana[k][1] = fft_half[k][1];
    }
    for (int k = bins; k < N; ++k) {
        ana[k][0] = 0.0;
        ana[k][1] = 0.0;
    }

    fftw_execute_dft(state.plan_c2c_inv, ana, ana);

    if (envelope) {
        for (int i = 0; i < samples_per_field; ++i) {
            double re = ana[i][0], im = ana[i][1];
            envelope[i] = std::sqrt(re * re + im * im);
        }
    }

    for (int i = 0; i < N; ++i) {
        ang[i] = std::atan2(ana[i][1], ana[i][0]);
    }

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

    std::memcpy(padded, demod, samples_per_field * sizeof(double));
    std::memset(padded + samples_per_field, 0, (N - samples_per_field) * sizeof(double));
    fftw_execute_dft_r2c(state.plan_r2c, padded, fft_half);

    for (int k = 0; k < bins; ++k) {
        double hr = fft_half[k][0], hi = fft_half[k][1];
        double fr = state.fvideo[k][0], fi = state.fvideo[k][1];
        post[k][0] = hr * fr - hi * fi;
        post[k][1] = hr * fi + hi * fr;
    }
    fftw_execute_dft_c2r(state.plan_c2r, post, padded);
    std::memcpy(demod, padded, samples_per_field * sizeof(double));

    for (int k = 0; k < bins; ++k) {
        double hr = fft_half[k][0], hi = fft_half[k][1];
        double fr = state.fvideo05[k][0], fi = state.fvideo05[k][1];
        post[k][0] = hr * fr - hi * fi;
        post[k][1] = hr * fi + hi * fr;
    }
    fftw_execute_dft_c2r(state.plan_c2r, post, padded);
    std::memcpy(demod05, padded, samples_per_field * sizeof(double));
}

// ---------------------------------------------------------------------------
// fm_demod -- full-field FFT demodulation
// ---------------------------------------------------------------------------

void fm_demod(FMDemodState& state,
              const double* raw,
              double* demod,
              double* demod05,
              double* envelope,
              int samples_per_field,
              const VideoFormat& fmt)
{
    fm_demod_fullfield(state, raw, demod, demod05, envelope, samples_per_field, fmt);
}

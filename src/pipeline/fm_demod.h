#pragma once
#include <fftw3.h>
#include "format/video_format.h"

// FM demodulation state for one worker thread.
//
// Each worker creates its own FMDemodState at init. FFTW plan creation
// is NOT thread-safe, but execution with different arrays IS thread-safe.
// All scratch buffers are owned by this struct and freed in destroy().
//
// [NTSC-SPECIFIC] Filter parameters (carrier frequencies, bandpass edges,
// deemphasis constants) are tuned for NTSC VHS SP.

struct FMDemodState {
    fftw_plan plan_r2c = nullptr;
    fftw_plan plan_c2c_inv = nullptr;
    fftw_plan plan_c2r = nullptr;

    int fft_size = 0;
    int freq_bins = 0;
    int spf = 0;  // samples_per_field this was initialized for

    // Pre-computed filter arrays
    double* rf_filter = nullptr;        // [freq_bins] reals
    fftw_complex* fvideo = nullptr;     // [freq_bins] complex
    fftw_complex* fvideo05 = nullptr;   // [freq_bins] complex

    // Scratch buffers (one field's worth)
    double* padded = nullptr;           // [fft_size]
    fftw_complex* fft_half = nullptr;   // [freq_bins]
    fftw_complex* analytic = nullptr;   // [fft_size]
    fftw_complex* post_fft = nullptr;   // [freq_bins]
    double* angles = nullptr;           // [fft_size]

    int f05_offset = 32;

    bool init(const VideoFormat& fmt, int target_samples = 0);
    void destroy();
    ~FMDemodState() { destroy(); }
};

// Process ONE field through FM demodulation.
// raw: input samples (samples_per_field doubles)
// demod: output video signal in Hz (samples_per_field doubles)
// demod05: output sync signal in Hz (samples_per_field doubles)
// envelope: output RF envelope magnitude (samples_per_field doubles, or nullptr to skip)
void fm_demod(FMDemodState& state,
              const double* raw,
              double* demod,
              double* demod05,
              double* envelope,       // nullptr to skip
              int samples_per_field,
              const VideoFormat& fmt);

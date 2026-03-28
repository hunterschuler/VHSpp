#pragma once
#include <fftw3.h>
#include "format/video_format.h"

// FM demodulation state for one worker thread.
//
// Each worker creates its own FMDemodState at init. FFTW plan creation
// is NOT thread-safe, but execution with different arrays IS thread-safe.
// All scratch buffers are owned by this struct and freed in destroy().
//
// Supports two modes:
//   Full-field: fft_size >= total samples → single FFT pass
//   Tiled:      fft_size < total samples  → overlap-save with cache-friendly tiles
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

    // Overlap-save tiling parameters (0 = full-field mode, no tiling)
    int tile_overlap = 0;    // overlap samples between adjacent tiles
    int tile_step = 0;       // valid output samples per tile = fft_size - tile_overlap

    // Pre-computed filter arrays
    double* rf_filter = nullptr;        // [freq_bins] reals
    fftw_complex* fvideo = nullptr;     // [freq_bins] complex
    fftw_complex* fvideo05 = nullptr;   // [freq_bins] complex

    // Scratch buffers (tile-sized)
    double* padded = nullptr;           // [fft_size]
    fftw_complex* fft_half = nullptr;   // [freq_bins]
    fftw_complex* analytic = nullptr;   // [fft_size]
    fftw_complex* post_fft = nullptr;   // [freq_bins]
    double* angles = nullptr;           // [fft_size]

    int f05_offset = 32;

    // Init with explicit tile size (0 = auto-size to target_samples).
    // tile_size > 0: use overlap-save with this FFT tile size.
    // tile_size = 0 and target_samples > 0: full-field FFT sized for target_samples.
    // tile_size = 0 and target_samples = 0: full-field FFT sized for spf.
    bool init(const VideoFormat& fmt, int target_samples = 0, int tile_size = 0);
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

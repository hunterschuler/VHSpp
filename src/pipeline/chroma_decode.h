#pragma once
#include <cstdint>
#include <fftw3.h>
#include "format/video_format.h"

// State carried across fields within a worker's chunk.
struct ChromaState {
    bool valid = false;
    int current_track = 0;          // 0 or 1
    double good_metric_threshold = 0.0;
    int cycle_start = 0;            // [NTSC-SPECIFIC] position in 8-field phase cycle
    int prev_quadrant = -1;         // burst phase quadrant from previous field
};

// Persistent FFTW plans + scratch for chroma processing.
// One per worker thread.
struct ChromaDemodState {
    // Pre-bandpass filter at capture rate
    fftw_plan bpf_plan_fwd = nullptr;
    fftw_plan bpf_plan_inv = nullptr;
    int raw_fft_size = 0;
    int raw_freq_bins = 0;
    double* bpf_filter = nullptr;       // [raw_freq_bins]
    double* bpf_padded = nullptr;       // [raw_fft_size]
    fftw_complex* bpf_fft = nullptr;    // [raw_freq_bins]

    // Per-line FFT at output rate
    fftw_plan line_plan_fwd = nullptr;
    fftw_plan line_plan_inv = nullptr;
    int line_fft_size = 0;
    int line_freq_bins = 0;
    double* line_buf = nullptr;         // [line_fft_size]
    fftw_complex* line_fft = nullptr;   // [line_freq_bins]

    bool init(const VideoFormat& fmt);
    void destroy();
    ~ChromaDemodState() { destroy(); }
};

// Decode chroma for ONE field.
// raw: raw RF signal at capture rate (for this field, samples_per_field)
// linelocs: line positions [lines_per_frame]
// scratch: writable buffer same size as raw (for bandpass-filtered signal)
// tbc_chroma: output uint16 [output_field_lines × output_line_len]
// is_first_field: from line_locs (1, 0, or -1)
// field_phase_id: output, NTSC phase ID 1-4
// chroma_state: carried across fields by worker
// demod_state: FFTW plans + scratch
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
                   ChromaDemodState& demod_state);

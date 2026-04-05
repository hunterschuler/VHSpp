#pragma once

#include <complex>
#include <optional>
#include <vector>

namespace vhsdecode::cppport {

struct SosSection {
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a0 = 1.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

struct SosFilter {
    std::vector<SosSection> sections;
};

struct IirFilter {
    std::vector<double> b;
    std::vector<double> a;
};

struct VideoEqConfig {
    IirFilter filter;
    double gain = 0.0;
    double sharpness_level = 0.0;
};

struct ChromaTrapConfig {
    double fs_hz = 0.0;
    double fsc_mhz = 0.0;
    int multiplier = 8;
    int delay = 4;
};

struct SubDeemphasisConfig {
    double deviation = 0.0;
    double exponential_scaling = 0.33;
    std::optional<double> scaling_1;
    std::optional<double> scaling_2;
    double logistic_mid = 0.0;
    double logistic_rate = 0.0;
    std::optional<double> static_factor;
};

struct NonLinearDeemphasisConfig {
    double highpass_limit_l = 0.0;
    double highpass_limit_h = 0.0;
};

struct DemodBlockFilters {
    std::size_t blocklen = 0;

    std::vector<std::complex<double>> rf_video;
    std::vector<std::complex<double>> hilbert;
    std::vector<std::complex<double>> fvideo_notch_f;

    std::vector<std::complex<double>> fvideo;
    std::vector<std::complex<double>> fvideo05;
    std::vector<std::complex<double>> nl_highpass_f;

    SosFilter fenv_post;
    SosFilter rf_top;
    SosFilter fvideo_burst;
    SosFilter nl_amplitude_lpf;

    std::optional<IirFilter> fvideo_notch;
    std::optional<IirFilter> chroma_audio_notch;
    std::optional<IirFilter> fsc_notch;

    int f05_offset = 32;
};

struct DemodBlockOptions {
    bool enable_video_notch = false;
    bool enable_high_boost = false;
    double high_boost = 0.0;

    bool disable_diff_demod = false;
    double diff_demod_check_value = 0.0;

    bool enable_video_eq = false;
    std::optional<VideoEqConfig> video_eq;
    bool enable_chroma_trap = false;
    std::optional<ChromaTrapConfig> chroma_trap;
    bool enable_nldeemp = false;
    std::optional<NonLinearDeemphasisConfig> nonlinear_deemphasis;
    bool enable_subdeemp = false;
    bool enable_fsc_notch = false;
    std::optional<SubDeemphasisConfig> sub_deemphasis;

    bool color_under = true;
    int chroma_offset = 0;
};

struct DemodBlockState {
    std::vector<double> video_eq_zi;
    bool video_eq_initialized = false;
};

struct DemodBlockInput {
    std::optional<std::vector<double>> data;
    std::optional<std::vector<std::complex<double>>> fftdata;
};

struct DemodBlockResult {
    std::vector<double> data;
    std::vector<double> env;
    std::vector<std::complex<double>> hilbert;
    std::vector<double> demod_pre_spike;
    std::vector<double> demod_diffed;
    std::vector<double> demod;
    std::vector<double> out_video;
    std::vector<double> out_video05;
    std::vector<double> out_chroma;
    std::vector<double> chroma_after_sos;
    std::vector<double> chroma_after_audio_notch;
    std::vector<double> chroma_after_video_notch;
};

// Literal-first C++ transliteration of vhsdecode.process.VHSDecode.demodblock().
//
// DIVERGENCE NOTES:
// - Python uses NumPy FFTs and Rust helpers (`vhsd_rust.unwrap_hilbert`,
//   `sosfiltfilt_rust`). The C++ port uses FFTW plus local equivalents.
// - Python `VideoEQ.filter_video()` is block-local; it does not preserve a
//   cross-block IIR state in DemodCacheTape. The optional state here exists
//   only for local scratch/reuse experiments and must not be threaded through
//   the live cache unless upstream behavior changes.
// - K1 filter/context construction now lives in `k1_context.h`; any remaining
//   construction-side divergences are documented there rather than here.
DemodBlockResult demodblock(const DemodBlockInput& input,
                            const DemodBlockFilters& filters,
                            const DemodBlockOptions& options,
                            double freq_hz,
                            DemodBlockState* state = nullptr);

}  // namespace vhsdecode::cppport

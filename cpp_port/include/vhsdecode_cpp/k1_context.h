#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "vhsdecode_cpp/demod_block.h"
#include "vhsdecode_cpp/chroma_afc.h"

namespace vhsdecode::cppport {

enum class CustomVideoFilterType {
    File,
    HighShelf,
    LowShelf,
};

struct CustomVideoFilterSpec {
    CustomVideoFilterType type = CustomVideoFilterType::File;
    std::string filename;
    double midfreq = 0.0;
    double gain = 0.0;
    double q = 0.0;
};

struct VideoEqDesignParams {
    double corner = 0.0;
    double transition = 0.0;
    int order_limit = 20;
};

struct K1DecoderParams {
    bool video_bpf_supergauss = false;
    double video_bpf_low = 0.0;
    double video_bpf_high = 0.0;
    int video_bpf_order = 0;

    int video_lpf_extra_order = 0;
    double video_lpf_extra = 0.0;
    int video_hpf_extra_order = 0;
    double video_hpf_extra = 0.0;

    std::optional<double> video_rf_peak_freq;
    double video_rf_peak_gain = 3.0;
    double video_rf_peak_bandwidth = 2.5e6;

    std::optional<double> fm_audio_channel_0_freq;
    std::optional<double> fm_audio_channel_1_freq;

    std::optional<double> start_rf_linear;
    std::optional<double> boost_rf_linear_0;
    double boost_rf_linear_20 = 1.0;
    bool boost_rf_linear_double = false;

    double boost_bpf_low = 0.0;
    double boost_bpf_high = 0.0;
    double boost_bpf_mult = 0.0;

    double deemph_gain = 0.0;
    double deemph_mid = 0.0;
    double deemph_q = 0.5;

    bool video_lpf_supergauss = false;
    double video_lpf_freq = 0.0;
    int video_lpf_order = 0;

    std::vector<CustomVideoFilterSpec> video_custom_luma_filters;

    double nonlinear_amp_lpf_freq = 700000.0;
    std::optional<double> nonlinear_bandpass_upper;
    int nonlinear_bandpass_order = 1;
    double nonlinear_highpass_freq = 0.0;
    double nonlinear_highpass_limit_l = 0.0;
    double nonlinear_highpass_limit_h = 0.0;

    bool use_sub_deemphasis = false;
    double nonlinear_exp_scaling = 0.25;
    std::optional<double> nonlinear_scaling_1;
    std::optional<double> nonlinear_scaling_2;
    std::optional<double> nonlinear_logistic_mid;
    std::optional<double> nonlinear_logistic_rate;
    std::optional<double> nonlinear_static_factor;
    std::optional<double> nonlinear_deviation;

    double color_under_carrier = 0.0;
    double chroma_bpf_upper = 0.0;
    int chroma_bpf_order = 4;
    double chroma_bpf_lower = 60000.0;
    std::optional<double> chroma_audio_notch_freq;

    std::optional<VideoEqDesignParams> video_eq;
};

struct K1RuntimeParams {
    std::optional<double> high_boost;
    bool disable_diff_demod = false;
    double diff_demod_check_value = 0.0;

    bool enable_video_notch = false;
    double video_notch_freq = 0.0;
    double video_notch_q = 10.0;

    bool enable_chroma_audio_notch = false;
    bool enable_chroma_trap = false;

    bool enable_nldeemp = false;
    bool enable_subdeemp = false;
    bool enable_fsc_notch = false;

    bool color_under = true;
    int chroma_offset = 0;

    double sharpness_level = 0.0;
    int fm_audio_notch_q = 0;

    bool do_cafc = false;
    std::optional<double> cafc_output_freq_half_hz;
};

struct K1BuildConfig {
    std::size_t blocklen = 0;
    double freq_hz = 0.0;
    double fsc_mhz = 0.0;
    double hz_ire = 0.0;
    double vsync_ire = 0.0;
    double fps = 0.0;
    int frame_lines = 0;
    int max_field_lines = 0;
    int outlinelen = 0;
    const char* tape_format = "VHS";

    K1DecoderParams decoder_params;
    K1RuntimeParams runtime;
};

struct K1Context {
    DemodBlockFilters filters;
    DemodBlockOptions options;
    DemodBlockState state;
    std::optional<ChromaAfc> chroma_afc;
    struct DebugComponents {
        std::vector<std::complex<double>> rf_bpf;
        std::vector<std::complex<double>> rf_extra_lpf;
        std::vector<std::complex<double>> rf_extra_hpf;
        std::vector<std::complex<double>> rf_peak;
        std::vector<std::complex<double>> rf_audio_notch;
        std::vector<std::complex<double>> rf_ramp;
        std::vector<std::complex<double>> filter_deemp;
        std::vector<std::complex<double>> filter_video_lpf;
        std::vector<std::complex<double>> filter_custom;
        std::vector<std::complex<double>> filter_05;
        std::vector<double> filter_05_taps;
    } debug;
};

// Literal-first C++ construction of the filter/context state that Python
// assembles in VHSDecode.__init__() + computevideofilters() +
// _computevideofilters_b() for K1/demodblock().
//
// DIVERGENCE NOTES:
// - SciPy's exact filter-design implementations (`butter`, `buttord`,
//   `firwin`, `iirnotch`, `freqz`, `sosfreqz`) do not exist in-tree for C++.
//   The builder therefore mirrors those algorithms locally. Any places where
//   it still fails to match SciPy closely enough are called out inline.
// - `ChromaAFC` is ported as a real module, but the builder still uses direct
//   C++ equivalents instead of Python object construction.
// - Custom filter files use Python's text complex format (`a+bj`). The C++
//   loader mirrors that file contract directly.
K1Context build_k1_context(const K1BuildConfig& config);

}  // namespace vhsdecode::cppport

#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "vhsdecode_cpp/demod_block.h"

namespace vhsdecode::cppport {

struct ChromaAfcConfig {
    double demod_rate_hz = 0.0;
    double under_ratio = 0.0;
    double fps = 0.0;
    int frame_lines = 0;
    int max_field_lines = 0;
    int outlinelen = 0;
    double fsc_mhz = 0.0;
    double color_under_carrier_hz = 0.0;
    int chroma_bandpass_order = 4;
    bool linearize = false;
    bool do_cafc = false;
    double chroma_bpf_lower_hz = 60000.0;
    const char* tape_format = "VHS";
};

struct ChromaAfcMeasurement {
    double spec_hz = 0.0;
    double measured_hz = 0.0;
    double long_term_offset_hz = 0.0;
    double cc_phase_rad = 0.0;
};

class ChromaAfc {
public:
    explicit ChromaAfc(const ChromaAfcConfig& config);

    double getSampleRate() const;
    double getOutFreqHalf() const;

    void setCC(double fcc_hz);
    double getCC() const;
    double getCCPhase() const;
    void setCCPhase(double phase_rad);
    void resetCCPhase();
    void resetCC();

    const std::vector<std::vector<float>>& getChromaHet() const;
    std::pair<const std::vector<float>&, const std::vector<float>&> getFSCWaves() const;

    SosFilter getChromaBandpass() const;
    SosFilter getChromaBandpassFinal(bool color_under_format = true) const;

    ChromaAfcMeasurement freqOffset(const std::vector<double>& chroma, bool adjustf = true);

private:
    ChromaAfcConfig config_;

    double fv_ = 0.0;
    double fh_ = 0.0;
    double cc_phase_ = 0.0;
    double power_threshold_ = 1.0 / 3.0;
    double transition_expand_ = 12.0;
    double max_f_dev_percent_down_ = 0.0;
    double max_f_dev_percent_up_ = 0.0;
    double out_sample_rate_mhz_ = 0.0;
    double samp_rate_hz_ = 0.0;
    double out_frequency_half_mhz_ = 0.0;
    int fieldlen_ = 0;

    double cc_freq_mhz_ = 0.0;

    std::vector<float> fsc_wave_;
    std::vector<float> fsc_cos_wave_;
    std::vector<std::vector<float>> chroma_heterodyne_;

    std::vector<double> corrector_{1.0, 0.0};
    bool on_linearization_ = false;

    class StackableMA {
    public:
        StackableMA(int min_watermark = 3, int window_average = 30);
        void push(double value);
        std::optional<double> pull();
        bool has_values() const;
        std::optional<double> current() const;
        int size() const;
        double work(double value);

    private:
        double mean() const;

        int window_average_;
        int min_watermark_;
        std::vector<double> stack_;
    };

    std::optional<StackableMA> meas_stack_;
    std::optional<StackableMA> chroma_log_drift_;
    std::optional<StackableMA> chroma_bias_drift_;

    std::vector<IirFilter> narrowband_filters_;

    void genHetC();
    std::vector<std::vector<float>> genHetCDirect() const;
    std::pair<double, double> getBandTolerance() const;
    double compensate(double x) const;
    double fineTune(double freq, double max_step) const;
    double measureCenterFreq(const std::vector<double>& data);
    double fftCenterFreq(const std::vector<double>& data);
    std::vector<std::vector<double>> tableset(int sample_size, int points = 256);
    void fit();
};

}  // namespace vhsdecode::cppport

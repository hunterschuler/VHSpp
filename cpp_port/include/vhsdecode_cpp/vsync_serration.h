#pragma once

#include <optional>
#include <utility>
#include <vector>

namespace vhsdecode::cppport {

struct VsyncSerrationSysParams {
    double fps = 0.0;
    int frame_lines = 0;
    double eq_pulse_us = 0.0;
};

struct VsyncSerrationConfig {
    double sample_rate_hz = 0.0;
    int divisor = 1;
    bool show_decoded_serration = false;
    VsyncSerrationSysParams sysparams;
};

struct VsyncEnvelopeDebug {
    std::vector<double> envelope;
    std::vector<int> minima;
    std::vector<int> serrations;
    std::vector<int> arbitrated;
    std::vector<int> serration_locs;
    int mask_len = 0;
    bool state = false;
};

class VsyncSerration {
public:
    explicit VsyncSerration(const VsyncSerrationConfig& config);

    int get_eq_pulselen() const;
    int get_line_len() const;

    std::optional<std::pair<double, double>> pull_levels() const;
    bool has_levels() const;
    bool has_serration() const;

    void push_levels(const std::pair<double, double>& levels);
    double mean_bias() const;
    std::vector<double> remove_bias(const std::vector<double>& data) const;

    void work(const std::vector<double>& data);

    double sync_level_bias() const { return sync_level_bias_; }
    const VsyncEnvelopeDebug& last_debug() const { return last_debug_; }

private:
    class StackableMA {
    public:
        StackableMA(int min_watermark = 3, int window_average = 30);
        void push(double value);
        std::optional<double> pull() const;
        bool has_values() const;
        int size() const;

    private:
        double mean() const;

        int window_average_ = 30;
        int min_watermark_ = 3;
        std::vector<double> stack_;
    };

    double samp_rate_ = 0.0;
    int divisor_ = 1;
    bool show_decoded_ = false;
    int eq_pulselen_ = 0;
    int vsynclen_ = 0;
    int linelen_ = 0;
    std::pair<double, double> vbi_time_range_{0.0, 0.0};

    std::vector<double> vsync_env_b_;
    std::vector<double> vsync_env_a_;
    std::vector<double> serration_base_lo_b_;
    std::vector<double> serration_base_lo_a_;
    std::vector<double> serration_base_hi_b_;
    std::vector<double> serration_base_hi_a_;
    std::vector<double> serration_envelope_b_;
    std::vector<double> serration_envelope_a_;

    StackableMA sync_levels_;
    StackableMA blank_levels_;
    double sync_level_bias_ = 0.0;
    int fieldcount_ = 0;
    bool found_serration_ = false;
    VsyncEnvelopeDebug last_debug_;

    std::pair<std::vector<double>, double> vsync_envelope_simple(
        const std::vector<double>& data) const;
    std::pair<std::vector<double>, double> vsync_envelope_double(
        const std::vector<double>& data) const;
    std::vector<int> power_ratio_search(const std::vector<double>& data) const;
    std::optional<std::vector<int>> vsync_arbitrage(
        const std::vector<int>& where_allmin,
        const std::vector<int>& serrations,
        int datalen) const;
    std::pair<bool, std::optional<std::pair<int, int>>> search_eq_pulses(
        const std::vector<double>& data,
        int pos,
        int linespan = 30);
    std::optional<bool> vsync_envelope(const std::vector<double>& data, int padding = 1024);
};

}  // namespace vhsdecode::cppport

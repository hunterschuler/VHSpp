#pragma once

#include "vhsdecode_cpp/resync_core.h"
#include "vhsdecode_cpp/resync_state.h"
#include "vhsdecode_cpp/vsync_serration.h"

#include <string>
#include <vector>

namespace vhsdecode_cpp {

struct ResyncRuntimeConfig {
    double sample_rate_hz = 0.0;
    double sample_rate_mhz = 0.0;
    int divisor = 1;
    double fps = 0.0;
    int frame_lines = 0;
    double eq_pulse_us = 0.0;
    double vsync_pulse_us = 0.0;
    SysParamsConst sysparams_const{};
};

struct ResyncFieldInput {
    std::vector<double> demod;
    std::vector<double> demod_05;
    std::string color_system = "NTSC";
    bool fallback_vsync = false;
    bool disable_dc_offset = false;
};

class ResyncRuntime {
public:
    explicit ResyncRuntime(const ResyncRuntimeConfig& config);

    std::vector<Pulse> get_pulses(ResyncFieldInput& field, bool check_levels = true);

    int eq_pulselen() const { return eq_pulselen_; }
    int linelen() const { return linelen_; }
    int long_pulse_max() const { return long_pulse_max_; }
    double last_pulse_threshold() const { return last_pulse_threshold_; }
    std::pair<std::optional<double>, std::optional<double>> field_state_levels() const {
        return field_state_.pull_levels();
    }
    std::optional<std::pair<double, double>> serration_levels() const {
        return vsync_serration_.pull_levels();
    }
    double sync_level_bias() const { return vsync_serration_.sync_level_bias(); }

private:
    bool level_check(
        double sync,
        double blank,
        const std::vector<double>& sync_reference,
        bool full = true) const;

    std::pair<double, double> pulses_levels(
        const ResyncFieldInput& field,
        const std::vector<Pulse>& pulses,
        double pulse_level,
        bool store_in_field_state);

    void add_pulselevels_to_serration_measures(
        const ResyncFieldInput& field,
        const std::vector<double>& demod_05,
        bool check_long);

    std::vector<Pulse> get_pulses_serration(
        ResyncFieldInput& field,
        bool check_levels);

    ResyncRuntimeConfig config_{};
    vhsdecode::cppport::VsyncSerration vsync_serration_;
    FieldState field_state_;
    int eq_pulselen_ = 0;
    int linelen_ = 0;
    int long_pulse_max_ = 0;
    double last_pulse_threshold_ = 0.0;
    bool use_serration_ = true;
};

}  // namespace vhsdecode_cpp

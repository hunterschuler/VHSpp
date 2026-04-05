#pragma once

#include <cstdint>
#include <vector>

namespace vhsdecode_cpp {

struct ValidPulsesToLinelocsResult {
    std::vector<double> line_locations;
    std::vector<std::uint8_t> line_location_errs;
    double last_valid_pulse_location = -1.0;
};

struct ValidPulse {
    int type = 0;
    int start = 0;
    int valid = 0;
};

struct RawPulse {
    int start = 0;
    int len = 0;
};

struct PulseTimingRanges {
    double hsync_min = 0.0;
    double hsync_max = 0.0;
    double eq_min = 0.0;
    double eq_max = 0.0;
    double vsync_min = 0.0;
    double vsync_max = 0.0;
};

bool pulse_qualitycheck(
    const ValidPulse& prev_pulse,
    int pulse_type,
    const RawPulse& pulse,
    int in_line_len);

struct VblankStateMachineResult {
    bool done = false;
    std::vector<ValidPulse> validpulses;
};

VblankStateMachineResult run_vblank_state_machine(
    const std::vector<RawPulse>& raw_pulses,
    const PulseTimingRanges& line_timings,
    int num_pulses,
    int in_line_len);

std::vector<ValidPulse> refine_pulses(
    std::vector<RawPulse>& raw_pulses,
    const std::vector<double>& demod_05,
    const PulseTimingRanges& line_timings,
    int num_pulses,
    int in_line_len,
    double ire0,
    double hz_ire,
    double eq_pulselen = 0.0,
    double long_pulse_max = 0.0);

struct FirstHsyncInput {
    std::vector<ValidPulse> validpulses;
    double meanlinelen = 0.0;
    bool is_ntsc = false;
    std::vector<int> field_lines;
    int num_eq_pulses = 0;
    int prev_first_field = -1;
    double last_field_offset_lines = 0.0;
    double prev_first_hsync_loc = -1.0;
    double prev_hsync_diff = 0.0;
    int field_order_confidence = 0;
    double fallback_line0loc = -1.0;
    int fallback_is_first_field = -1;
    int fallback_is_first_field_confidence = -1;
};

struct FirstHsyncResult {
    bool has_line0loc = false;
    double line0loc = 0.0;
    bool has_first_hsync_loc = false;
    double first_hsync_loc = 0.0;
    double hsync_start_line = 0.0;
    bool has_next_field = false;
    double next_field = 0.0;
    bool first_field = false;
    bool progressive_field = false;
    double prev_hsync_diff = 0.0;
    std::vector<int> vblank_pulses;
    int first_field_confidence = 0;
    int second_field_confidence = 0;
    int progressive_field_confidence = 0;
    std::vector<double> field_order_lengths;
};

FirstHsyncResult get_first_hsync_loc(const FirstHsyncInput& input);

ValidPulsesToLinelocsResult valid_pulses_to_linelocs(
    const std::vector<double>& valid_pulses_in,
    int reference_pulse,
    int reference_line,
    double meanlinelen,
    double hsync_tolerance,
    int proclines,
    double gap_detection_threshold);

struct HsyncRefineInput {
    std::vector<double> linelocs1;
    std::vector<double> demod_05;
    int normal_hsync_length = 0;
    int one_usec = 0;
    double sample_rate_mhz = 0.0;
    bool is_pal = false;
    bool disable_right_hsync = false;
    double hsync_threshold = 0.0;
    double ire_30 = 0.0;
    double ire_n_65 = 0.0;
    double ire_110 = 0.0;
};

std::vector<double> refine_linelocs_hsync(
    const HsyncRefineInput& input,
    std::vector<std::uint8_t>& linebad);

}  // namespace vhsdecode_cpp

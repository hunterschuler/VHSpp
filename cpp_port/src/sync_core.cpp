#include "vhsdecode_cpp/sync_core.h"
#include "vhsdecode_cpp/resync_core.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace vhsdecode_cpp {
namespace {

constexpr double kNoneDouble = std::numeric_limits<double>::quiet_NaN();
constexpr int kNoneInt = -1;

inline bool is_none_double(double value) {
    return std::isnan(value);
}

inline int round_to_int(double value) {
    return static_cast<int>(std::lround(value));
}

inline double c_abs(double value) {
    return value < 0.0 ? -value : value;
}

inline bool inrange(double value, double mi, double ma) {
    return value >= mi && value <= ma;
}

double round_nearest_line_loc(double line_number) {
    return std::round(0.5 * std::round(line_number / 0.5) * 10.0) / 10.0;
}

struct SyncDistanceInput {
    double meanlinelen = 0.0;
    double vsync_tolerance_lines = 0.5;
    double hsync_start_line = 0.0;
    double first_pulse = -1.0;
    double second_pulse = -1.0;
    double first_line = -1.0;
    double second_line = -1.0;
};

struct SyncDistanceOutput {
    double distance_offset = 0.0;
    double hsync_loc = 0.0;
    int valid_locations = 0;
};

void calc_sync_from_known_distances(
    const SyncDistanceInput& input,
    SyncDistanceOutput& output) {
    output.distance_offset = 0.0;
    output.hsync_loc = 0.0;
    output.valid_locations = 0;

    if (input.first_pulse != -1.0 && input.second_pulse != -1.0 && input.meanlinelen != 0.0) {
        const double actual_lines =
            (input.first_pulse - input.second_pulse) / input.meanlinelen;
        const double expected_lines = input.first_line - input.second_line;
        if (actual_lines < expected_lines + input.vsync_tolerance_lines &&
            actual_lines > expected_lines - input.vsync_tolerance_lines) {
            output.distance_offset = actual_lines - expected_lines;
            output.hsync_loc =
                input.second_pulse +
                input.meanlinelen * (input.hsync_start_line - input.second_line);
            output.valid_locations = 1;
        }
    }
}

double c_median_mean_proxy(
    const std::vector<double>& data,
    std::size_t begin,
    std::size_t end) {
    // Literal port note: sync.pyx's c_median is actually a mean, not a true median.
    // We keep that behavior for parity.
    if (begin >= end || end > data.size()) {
        return 0.0;
    }
    double acc = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
        acc += data[i];
    }
    return acc / static_cast<double>(end - begin);
}

double c_max(
    const std::vector<double>& data,
    std::size_t begin,
    std::size_t end) {
    if (begin >= end || end > data.size()) {
        return kNoneDouble;
    }
    double result = kNoneDouble;
    for (std::size_t i = begin; i < end; ++i) {
        if (data[i] > result) {
            result = data[i];
        }
    }
    return result;
}

bool is_out_of_range(
    const std::vector<double>& data,
    std::size_t begin,
    std::size_t end,
    double min_value,
    double max_value) {
    if (begin >= end || end > data.size()) {
        return true;
    }
    for (std::size_t i = begin; i < end; ++i) {
        if (data[i] < min_value || data[i] > max_value) {
            return true;
        }
    }
    return false;
}

int calczc_findfirst(
    const std::vector<double>& data,
    std::size_t begin,
    std::size_t end,
    double target,
    bool rising) {
    if (begin >= end || end > data.size()) {
        return kNoneInt;
    }
    for (std::size_t i = begin; i < end; ++i) {
        if (rising) {
            if (data[i] >= target) {
                return static_cast<int>(i - begin);
            }
        } else if (data[i] <= target) {
            return static_cast<int>(i - begin);
        }
    }
    return kNoneInt;
}

double calczc_do(
    const std::vector<double>& data,
    int start_offset_raw,
    double target,
    int count = 10,
    int edge = 0) {
    if (data.empty()) {
        return kNoneDouble;
    }

    const int start_offset = std::max(1, start_offset_raw);
    if (start_offset_raw < 0 || start_offset >= static_cast<int>(data.size())) {
        return kNoneDouble;
    }

    const int icount = count + 1;

    if (edge == 0) {
        edge = data[start_offset] < target ? 1 : -1;
    }

    if (edge == 1) {
        if (data[start_offset_raw] > target) {
            return kNoneDouble;
        }
    } else if (edge == -1) {
        if (data[start_offset_raw] < target) {
            return kNoneDouble;
        }
    }

    const std::size_t begin = static_cast<std::size_t>(start_offset);
    const std::size_t end = std::min<std::size_t>(
        data.size(),
        static_cast<std::size_t>(start_offset + icount));
    const int loc = calczc_findfirst(data, begin, end, target, edge == 1);
    if (loc == kNoneInt) {
        return kNoneDouble;
    }

    const int x = start_offset + loc;
    if (x <= 0 || x >= static_cast<int>(data.size())) {
        return kNoneDouble;
    }

    const double a = data[static_cast<std::size_t>(x - 1)] - target;
    const double b = data[static_cast<std::size_t>(x)] - target;
    const double y = (b - a != 0.0) ? (-a / (-a + b)) : 0.0;
    return static_cast<double>(x - 1) + y;
}

std::size_t clamp_index(int index, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    if (index <= 0) {
        return 0;
    }
    const auto uindex = static_cast<std::size_t>(index);
    return std::min(uindex, size);
}

}  // namespace

bool pulse_qualitycheck(
    const ValidPulse& prev_pulse,
    int pulse_type,
    const RawPulse& pulse,
    int in_line_len) {
    double range_min = 0.4;
    double range_max = 1.1;
    if (prev_pulse.type > 0 && pulse_type > 0) {
        range_min = 0.4;
        range_max = 0.6;
    } else if (prev_pulse.type == 0 && pulse_type == 0) {
        range_min = 0.9;
        range_max = 1.1;
    }
    const double linelen =
        (static_cast<double>(pulse.start) - static_cast<double>(prev_pulse.start)) /
        static_cast<double>(in_line_len);
    return inrange(linelen, range_min, range_max);
}

VblankStateMachineResult run_vblank_state_machine(
    const std::vector<RawPulse>& raw_pulses,
    const PulseTimingRanges& line_timings,
    int num_pulses,
    int in_line_len) {
    VblankStateMachineResult result;
    const double num_pulses_half = static_cast<double>(num_pulses) / 2.0;

    std::vector<std::pair<int, int>> vsyncs;
    int vsync_start = -1;
    int state_end = 0;
    double state_length = 0.0;
    bool has_state_length = false;

    constexpr int HSYNC = 0;
    constexpr int EQPL1 = 1;
    constexpr int VSYNC = 2;
    constexpr int EQPL2 = 3;

    for (const auto& p : raw_pulses) {
        int state = result.validpulses.empty() ? -1 : result.validpulses.back().type;
        int spulse_type = -1;

        if (state == -1) {
            if (inrange(p.len, line_timings.hsync_min, line_timings.hsync_max)) {
                spulse_type = HSYNC;
            }
        } else if (state == HSYNC) {
            if (inrange(p.len, line_timings.hsync_min, line_timings.hsync_max)) {
                spulse_type = HSYNC;
            } else if (inrange(p.len, line_timings.eq_min, line_timings.eq_max)) {
                spulse_type = EQPL1;
                state_length = num_pulses_half;
                has_state_length = true;
            } else if (inrange(p.len, line_timings.vsync_min, line_timings.vsync_max)) {
                vsync_start = static_cast<int>(result.validpulses.size()) - 1;
                spulse_type = VSYNC;
            }
        } else if (state == EQPL1) {
            if (inrange(p.len, line_timings.eq_min, line_timings.eq_max)) {
                spulse_type = EQPL1;
            } else if (inrange(p.len, line_timings.vsync_min, line_timings.vsync_max)) {
                vsync_start = static_cast<int>(result.validpulses.size()) - 1;
                spulse_type = VSYNC;
                state_length = num_pulses_half;
                has_state_length = true;
            } else if (inrange(p.len, line_timings.hsync_min, line_timings.hsync_max)) {
                spulse_type = HSYNC;
            }
        } else if (state == VSYNC) {
            if (inrange(p.len, line_timings.eq_min, line_timings.eq_max)) {
                vsyncs.push_back({vsync_start, static_cast<int>(result.validpulses.size()) - 1});
                spulse_type = EQPL2;
                state_length = num_pulses_half;
                has_state_length = true;
            } else if (inrange(p.len, line_timings.vsync_min, line_timings.vsync_max)) {
                spulse_type = VSYNC;
            } else if (p.start > state_end &&
                       inrange(p.len, line_timings.hsync_min, line_timings.hsync_max)) {
                spulse_type = HSYNC;
            }
        } else if (state == EQPL2) {
            if (inrange(p.len, line_timings.eq_min, line_timings.eq_max)) {
                spulse_type = EQPL2;
            } else if (inrange(p.len, line_timings.hsync_min, line_timings.hsync_max)) {
                spulse_type = HSYNC;
                result.done = true;
            }
        }

        if (spulse_type != -1 && spulse_type != state) {
            if (p.start < state_end) {
                spulse_type = -1;
            } else if (has_state_length) {
                state_end = static_cast<int>(
                    static_cast<double>(p.start) + ((state_length - 0.1) * in_line_len));
                has_state_length = false;
            }
        }

        if (spulse_type != -1) {
            ValidPulse vp{};
            vp.type = spulse_type;
            vp.start = p.start;
            vp.valid = result.validpulses.empty()
                ? 0
                : (pulse_qualitycheck(result.validpulses.back(), spulse_type, p, in_line_len) ? 1 : 0);
            result.validpulses.push_back(vp);
        }

        if (result.done) {
            return result;
        }
    }

    return result;
}

std::vector<ValidPulse> refine_pulses(
    std::vector<RawPulse>& raw_pulses,
    const std::vector<double>& demod_05,
    const PulseTimingRanges& line_timings,
    int num_pulses,
    int in_line_len,
    double ire0,
    double hz_ire,
    double eq_pulselen,
    double long_pulse_max) {
    constexpr int HSYNC = 0;
    std::vector<ValidPulse> valid_pulses;

    const double rescue_min_synclen =
        eq_pulselen > 0.0 ? (eq_pulselen * 1.0 / 8.0) : (line_timings.hsync_min / 8.0);
    const double rescue_max_synclen =
        long_pulse_max > 0.0 ? long_pulse_max : (line_timings.hsync_max * 5.0);

    std::size_t i = 0;
    while (i < raw_pulses.size()) {
        auto curpulse = raw_pulses[i];
        if (inrange(curpulse.len, line_timings.hsync_min, line_timings.hsync_max)) {
            ValidPulse vp{};
            vp.type = HSYNC;
            vp.start = curpulse.start;
            vp.valid = valid_pulses.empty()
                ? 0
                : (pulse_qualitycheck(valid_pulses.back(), HSYNC, curpulse, in_line_len) ? 1 : 0);
            valid_pulses.push_back(vp);
            ++i;
        } else if (inrange(curpulse.len, line_timings.hsync_max, line_timings.hsync_max * 3.0)) {
            const int start = std::max(0, curpulse.start);
            const int end = std::min<int>(static_cast<int>(demod_05.size()), curpulse.start + curpulse.len);
            if (start < end) {
                const double threshold =
                    ire0 + (hz_ire * (((demod_05[static_cast<std::size_t>(start)] - ire0) / hz_ire) - 10.0));
                std::vector<RawPulse> local_pulses;
                std::vector<int> starts;
                std::vector<int> lengths;
                std::vector<double> data(
                    demod_05.begin() + start,
                    demod_05.begin() + end);
                vhsdecode_cpp::findpulses_numba_raw(
                    data,
                    threshold,
                    rescue_min_synclen,
                    rescue_max_synclen,
                    starts,
                    lengths);
                if (!starts.empty()) {
                    raw_pulses[i] = RawPulse{curpulse.start + starts[0], lengths[0]};
                } else {
                    ++i;
                }
            } else {
                ++i;
            }
        } else if (
            i > 2 &&
            inrange(raw_pulses[i].len, line_timings.eq_min, line_timings.eq_max) &&
            !valid_pulses.empty() &&
            valid_pulses.back().type == HSYNC) {
            const auto begin = i - 2;
            const auto end = std::min<std::size_t>(raw_pulses.size(), i + 24);
            std::vector<RawPulse> window(raw_pulses.begin() + static_cast<std::ptrdiff_t>(begin),
                                         raw_pulses.begin() + static_cast<std::ptrdiff_t>(end));
            const auto state_res =
                run_vblank_state_machine(window, line_timings, num_pulses, in_line_len);
            if (state_res.done) {
                for (std::size_t j = 2; j < state_res.validpulses.size(); ++j) {
                    valid_pulses.push_back(state_res.validpulses[j]);
                }
                i += state_res.validpulses.size() - 2;
            } else {
                ++i;
            }
        } else {
            ++i;
        }
    }

    return valid_pulses;
}

FirstHsyncResult get_first_hsync_loc(const FirstHsyncInput& input) {
    FirstHsyncResult result;
    result.vblank_pulses.assign(8, -1);
    result.field_order_lengths.assign(4, -1.0);

    constexpr double kVsyncToleranceLines = 0.5;
    std::vector<double> field_order_lengths(4, -1.0);
    std::vector<double> vblank_lines(8, -1.0);

    constexpr int FIRST_VBLANK_EQ_1_START = 0;
    constexpr int FIRST_VBLANK_VSYNC_START = 1;
    constexpr int FIRST_VBLANK_VSYNC_END = 2;
    constexpr int FIRST_VBLANK_EQ_2_END = 3;
    constexpr int LAST_VBLANK_EQ_1_START = 4;
    constexpr int LAST_VBLANK_VSYNC_START = 5;
    constexpr int LAST_VBLANK_VSYNC_END = 6;
    constexpr int LAST_VBLANK_EQ_2_END = 7;

    if (input.field_lines.size() < 2) {
        throw std::invalid_argument("get_first_hsync_loc requires two field_lines values");
    }

    int last_pulse = -1;
    int group = 0;
    int field_group = 0;
    for (std::size_t i = 0; i < input.validpulses.size(); ++i) {
        if (last_pulse != -1 && input.validpulses[i].valid) {
            if (group == 0 &&
                input.validpulses[i].start >
                    static_cast<int>(input.validpulses[0].start +
                                     input.field_lines[0] * input.meanlinelen)) {
                group = 4;
                field_group = 2;
            }

            const auto& prev = input.validpulses[static_cast<std::size_t>(last_pulse)];
            const auto& curr = input.validpulses[i];
            if (prev.type == 0 && curr.type > 0) {
                result.vblank_pulses[static_cast<std::size_t>(0 + group)] = curr.start;
                field_order_lengths[static_cast<std::size_t>(0 + field_group)] =
                    round_nearest_line_loc(
                        (curr.start - prev.start) / input.meanlinelen);
            } else if (prev.type == 1 && curr.type == 2) {
                result.vblank_pulses[static_cast<std::size_t>(1 + group)] = curr.start;
            } else if (prev.type == 2 && curr.type == 3) {
                result.vblank_pulses[static_cast<std::size_t>(2 + group)] = curr.start;
            } else if (prev.type > 0 && curr.type == 0) {
                result.vblank_pulses[static_cast<std::size_t>(3 + group)] = prev.start;
                field_order_lengths[static_cast<std::size_t>(1 + field_group)] =
                    round_nearest_line_loc(
                        (curr.start - prev.start) / input.meanlinelen);
            }
        }
        last_pulse = static_cast<int>(i);
    }

    constexpr std::size_t FIRST_HSYNC_LENGTH = 0;
    constexpr std::size_t FIRST_EQPL2_LENGTH = 1;
    constexpr std::size_t LAST_HSYNC_LENGTH = 2;
    constexpr std::size_t LAST_EQPL2_LENGTH = 3;

    std::vector<double> first_field_lengths(4, -1.0);
    std::vector<double> second_field_lengths(4, -1.0);
    std::vector<double> progressive_field_lengths(4, -1.0);

    if (input.is_ntsc) {
        first_field_lengths[FIRST_HSYNC_LENGTH] = 1.0;
        first_field_lengths[FIRST_EQPL2_LENGTH] = 0.5;
        first_field_lengths[LAST_HSYNC_LENGTH] = 0.5;
        first_field_lengths[LAST_EQPL2_LENGTH] = 1.0;

        second_field_lengths[FIRST_HSYNC_LENGTH] = 0.5;
        second_field_lengths[FIRST_EQPL2_LENGTH] = 1.0;
        second_field_lengths[LAST_HSYNC_LENGTH] = 1.0;
        second_field_lengths[LAST_EQPL2_LENGTH] = 0.5;

        progressive_field_lengths[FIRST_HSYNC_LENGTH] = 1.0;
        progressive_field_lengths[FIRST_EQPL2_LENGTH] = 0.5;
        progressive_field_lengths[LAST_HSYNC_LENGTH] = 1.0;
        progressive_field_lengths[LAST_EQPL2_LENGTH] = 0.5;
    } else {
        first_field_lengths[FIRST_HSYNC_LENGTH] = 0.5;
        first_field_lengths[FIRST_EQPL2_LENGTH] = 0.5;
        first_field_lengths[LAST_HSYNC_LENGTH] = 1.0;
        first_field_lengths[LAST_EQPL2_LENGTH] = 1.0;

        second_field_lengths[FIRST_HSYNC_LENGTH] = 1.0;
        second_field_lengths[FIRST_EQPL2_LENGTH] = 1.0;
        second_field_lengths[LAST_HSYNC_LENGTH] = 0.5;
        second_field_lengths[LAST_EQPL2_LENGTH] = 0.5;

        progressive_field_lengths[FIRST_HSYNC_LENGTH] = 0.5;
        progressive_field_lengths[FIRST_EQPL2_LENGTH] = 0.5;
        progressive_field_lengths[LAST_HSYNC_LENGTH] = 0.5;
        progressive_field_lengths[LAST_EQPL2_LENGTH] = 0.5;
    }

    double interlaced_field_boundaries_consensus = 0.0;
    double interlaced_field_boundaries_detected = 0.0;
    double progressive_field_consensus = 0.0;
    double progressive_field_boundaries_detected = 0.0;
    for (std::size_t i = 0; i < field_order_lengths.size(); ++i) {
        const double field_length = field_order_lengths[i];
        if (field_length == first_field_lengths[i]) {
            interlaced_field_boundaries_consensus += 1.0;
            interlaced_field_boundaries_detected += 1.0;
        }
        if (field_length == second_field_lengths[i]) {
            interlaced_field_boundaries_detected += 1.0;
        }
        if (field_length == progressive_field_lengths[i]) {
            progressive_field_consensus += 1.0;
            progressive_field_boundaries_detected += 1.0;
        }
        if (field_length != -1.0) {
            progressive_field_boundaries_detected += 1.0;
        }
    }

    bool first_field = false;
    bool progressive_field = false;
    if (input.prev_first_field == -1) {
        if (interlaced_field_boundaries_detected == 0.0 ||
            std::round(interlaced_field_boundaries_consensus /
                       interlaced_field_boundaries_detected) == 1.0 ||
            input.fallback_is_first_field == 1) {
            first_field = true;
        } else {
            first_field = false;
        }
    } else {
        first_field = !static_cast<bool>(input.prev_first_field);
    }

    int field_order_confidence = input.field_order_confidence;
    int first_field_confidence = 0;
    int second_field_confidence = 0;
    const double interlaced_field_order_weighting =
        interlaced_field_boundaries_detected / static_cast<double>(field_order_lengths.size());
    int progressive_field_confidence = 0;
    const double progressive_field_order_weighting =
        progressive_field_boundaries_detected / static_cast<double>(field_order_lengths.size());
    if (interlaced_field_boundaries_detected > 0.0) {
        if (input.fallback_line0loc == -1.0 && input.prev_first_hsync_loc < 0.0) {
            field_order_confidence =
                field_order_confidence > 50 ? 50 : field_order_confidence;
        }
        first_field_confidence = round_to_int(
            (interlaced_field_boundaries_consensus / interlaced_field_boundaries_detected) *
            interlaced_field_order_weighting * 100.0);
        second_field_confidence = round_to_int(
            ((interlaced_field_boundaries_detected - interlaced_field_boundaries_consensus) /
             interlaced_field_boundaries_detected) *
            interlaced_field_order_weighting * 100.0);

        if (first_field_confidence >= field_order_confidence &&
            first_field_confidence > second_field_confidence) {
            first_field = true;
        } else if (second_field_confidence >= field_order_confidence &&
                   first_field_confidence < second_field_confidence) {
            first_field = false;
        }

        if (progressive_field_boundaries_detected > 0.0) {
            progressive_field_confidence = round_to_int(
                ((progressive_field_boundaries_detected - progressive_field_consensus) /
                 progressive_field_boundaries_detected) *
                progressive_field_order_weighting * 100.0);
            if (progressive_field_confidence ==
                static_cast<int>(field_order_lengths.size())) {
                progressive_field = true;
            }
        }
    }

    if (input.fallback_is_first_field_confidence > first_field_confidence &&
        input.fallback_is_first_field_confidence > second_field_confidence) {
        first_field = input.fallback_is_first_field == 1;
    }
    result.first_field_confidence = first_field_confidence;
    result.second_field_confidence = second_field_confidence;
    result.progressive_field_confidence = progressive_field_confidence;
    result.field_order_lengths = field_order_lengths;

    double line0loc_line = 0.0;
    const double vsync_section_lines = input.num_eq_pulses / 2.0;
    double hsync_start_line = 0.0;
    double current_field_lines = 0.0;
    double previous_field_lines = 0.0;
    const std::vector<double>& current_field_lengths =
        first_field ? first_field_lengths : second_field_lengths;
    if (first_field) {
        previous_field_lines = input.field_lines[1];
        current_field_lines = input.field_lines[0];
    } else {
        previous_field_lines = input.field_lines[0];
        current_field_lines = input.field_lines[1];
    }

    vblank_lines[FIRST_VBLANK_EQ_1_START] =
        line0loc_line + current_field_lengths[FIRST_HSYNC_LENGTH];
    vblank_lines[FIRST_VBLANK_VSYNC_START] =
        vblank_lines[FIRST_VBLANK_EQ_1_START] + vsync_section_lines;
    vblank_lines[FIRST_VBLANK_VSYNC_END] =
        vblank_lines[FIRST_VBLANK_VSYNC_START] + vsync_section_lines;
    vblank_lines[FIRST_VBLANK_EQ_2_END] =
        vblank_lines[FIRST_VBLANK_VSYNC_END] + vsync_section_lines - 0.5;

    hsync_start_line =
        vblank_lines[FIRST_VBLANK_EQ_2_END] +
        current_field_lengths[FIRST_EQPL2_LENGTH];

    vblank_lines[LAST_VBLANK_EQ_1_START] =
        current_field_lines + current_field_lengths[LAST_HSYNC_LENGTH];
    vblank_lines[LAST_VBLANK_VSYNC_START] =
        vblank_lines[LAST_VBLANK_EQ_1_START] + vsync_section_lines;
    vblank_lines[LAST_VBLANK_VSYNC_END] =
        vblank_lines[LAST_VBLANK_VSYNC_START] + vsync_section_lines;
    vblank_lines[LAST_VBLANK_EQ_2_END] =
        vblank_lines[LAST_VBLANK_VSYNC_END] + vsync_section_lines - 0.5;

    SyncDistanceInput sync_distance_input{};
    sync_distance_input.meanlinelen = input.meanlinelen;
    sync_distance_input.vsync_tolerance_lines = kVsyncToleranceLines;
    sync_distance_input.hsync_start_line = hsync_start_line;
    SyncDistanceOutput sync_distance_output{};

    double first_vblank_first_hsync_loc = 0.0;
    int first_vblank_valid_location_count = 0;
    double first_vblank_offset = 0.0;
    const std::vector<int> first_vblank_pulse_indexes = {
        FIRST_VBLANK_EQ_1_START,
        FIRST_VBLANK_VSYNC_START,
        FIRST_VBLANK_VSYNC_END,
        FIRST_VBLANK_EQ_2_END};
    for (std::size_t first_index = 0; first_index < first_vblank_pulse_indexes.size(); ++first_index) {
        for (std::size_t second_index = first_index + 1;
             second_index < first_vblank_pulse_indexes.size();
             ++second_index) {
            sync_distance_input.first_pulse =
                result.vblank_pulses[static_cast<std::size_t>(first_vblank_pulse_indexes[first_index])];
            sync_distance_input.second_pulse =
                result.vblank_pulses[static_cast<std::size_t>(first_vblank_pulse_indexes[second_index])];
            sync_distance_input.first_line =
                vblank_lines[static_cast<std::size_t>(first_vblank_pulse_indexes[first_index])];
            sync_distance_input.second_line =
                vblank_lines[static_cast<std::size_t>(first_vblank_pulse_indexes[second_index])];
            calc_sync_from_known_distances(sync_distance_input, sync_distance_output);
            first_vblank_offset += sync_distance_output.distance_offset;
            first_vblank_first_hsync_loc += sync_distance_output.hsync_loc;
            first_vblank_valid_location_count += sync_distance_output.valid_locations;
        }
    }

    double last_vblank_first_hsync_loc = 0.0;
    int last_vblank_valid_location_count = 0;
    double last_vblank_offset = 0.0;
    const std::vector<int> last_vblank_pulse_indexes = {
        LAST_VBLANK_EQ_1_START,
        LAST_VBLANK_VSYNC_START,
        LAST_VBLANK_VSYNC_END,
        LAST_VBLANK_EQ_2_END};
    for (std::size_t first_index = 0; first_index < last_vblank_pulse_indexes.size(); ++first_index) {
        for (std::size_t second_index = first_index + 1;
             second_index < last_vblank_pulse_indexes.size();
             ++second_index) {
            sync_distance_input.first_pulse =
                result.vblank_pulses[static_cast<std::size_t>(last_vblank_pulse_indexes[first_index])];
            sync_distance_input.second_pulse =
                result.vblank_pulses[static_cast<std::size_t>(last_vblank_pulse_indexes[second_index])];
            sync_distance_input.first_line =
                vblank_lines[static_cast<std::size_t>(last_vblank_pulse_indexes[first_index])];
            sync_distance_input.second_line =
                vblank_lines[static_cast<std::size_t>(last_vblank_pulse_indexes[second_index])];
            calc_sync_from_known_distances(sync_distance_input, sync_distance_output);
            last_vblank_offset += sync_distance_output.distance_offset;
            last_vblank_first_hsync_loc += sync_distance_output.hsync_loc;
            last_vblank_valid_location_count += sync_distance_output.valid_locations;
        }
    }

    double first_hsync_loc = 0.0;
    int valid_location_count = 0;
    double offset = 0.0;
    const double first_vblank_hsync_estimate =
        first_vblank_valid_location_count != 0
            ? first_vblank_first_hsync_loc / first_vblank_valid_location_count
            : 0.0;
    const double last_vblank_hsync_estimate =
        last_vblank_valid_location_count != 0
            ? last_vblank_first_hsync_loc / last_vblank_valid_location_count
            : 0.0;
    if (first_vblank_valid_location_count != 0 &&
        last_vblank_valid_location_count != 0 &&
        first_vblank_hsync_estimate <
            last_vblank_hsync_estimate + kVsyncToleranceLines * input.meanlinelen &&
        first_vblank_hsync_estimate >
            last_vblank_hsync_estimate - kVsyncToleranceLines * input.meanlinelen) {
        first_hsync_loc = first_vblank_first_hsync_loc + last_vblank_first_hsync_loc;
        valid_location_count =
            first_vblank_valid_location_count + last_vblank_valid_location_count;
        offset = first_vblank_offset + last_vblank_offset;
        for (int first_index : first_vblank_pulse_indexes) {
            for (int second_index : last_vblank_pulse_indexes) {
                sync_distance_input.first_pulse =
                    result.vblank_pulses[static_cast<std::size_t>(first_index)];
                sync_distance_input.second_pulse =
                    result.vblank_pulses[static_cast<std::size_t>(second_index)];
                sync_distance_input.first_line =
                    vblank_lines[static_cast<std::size_t>(first_index)];
                sync_distance_input.second_line =
                    vblank_lines[static_cast<std::size_t>(second_index)];
                calc_sync_from_known_distances(sync_distance_input, sync_distance_output);
                offset += sync_distance_output.distance_offset;
                first_hsync_loc += sync_distance_output.hsync_loc;
                valid_location_count += sync_distance_output.valid_locations;
            }
        }
    } else if (input.fallback_line0loc != -1.0) {
        first_hsync_loc = input.fallback_line0loc + input.meanlinelen * hsync_start_line;
        valid_location_count = 1;
        offset = 0.0;
    } else if (
        first_vblank_valid_location_count == 6 ||
        (input.prev_first_hsync_loc <= 0.0 &&
         first_vblank_valid_location_count != 0 &&
         first_vblank_valid_location_count > last_vblank_valid_location_count)) {
        first_hsync_loc = first_vblank_first_hsync_loc;
        valid_location_count = first_vblank_valid_location_count;
        offset = first_vblank_offset;
    } else if (
        last_vblank_valid_location_count == 6 ||
        (input.prev_first_hsync_loc <= 0.0 &&
         last_vblank_valid_location_count != 0 &&
         last_vblank_valid_location_count > first_vblank_valid_location_count)) {
        first_hsync_loc = last_vblank_first_hsync_loc;
        valid_location_count = last_vblank_valid_location_count;
        offset = last_vblank_offset;
    }

    const double estimated_hsync_field_lines =
        input.is_ntsc ? previous_field_lines : current_field_lines;
    const int estimated_hsync_loc = round_to_int(
        (input.last_field_offset_lines + estimated_hsync_field_lines +
         input.prev_first_hsync_loc / input.meanlinelen) *
        input.meanlinelen);

    double estimated_hsync_with_offset = 0.0;
    bool used_estimated_hsync = false;
    double prev_hsync_diff = input.prev_hsync_diff;
    if (valid_location_count == 0 && input.prev_first_hsync_loc > 0.0) {
        if (prev_hsync_diff <= 0.5 && prev_hsync_diff >= -0.5) {
            estimated_hsync_with_offset =
                estimated_hsync_loc + input.meanlinelen * prev_hsync_diff;
        } else {
            estimated_hsync_with_offset = estimated_hsync_loc;
        }
        if (estimated_hsync_with_offset <= 0.0) {
            estimated_hsync_with_offset =
                !input.validpulses.empty() ? input.validpulses.front().start : 0.0;
        }
        first_hsync_loc += estimated_hsync_with_offset;
        valid_location_count += 1;
        used_estimated_hsync = true;
    }

    result.hsync_start_line = hsync_start_line;
    result.first_field = first_field;
    result.progressive_field = progressive_field;
    result.prev_hsync_diff = prev_hsync_diff;

    if (valid_location_count > 0) {
        offset /= valid_location_count;
        first_hsync_loc = std::round((first_hsync_loc + offset) / valid_location_count);
        if (!used_estimated_hsync) {
            prev_hsync_diff = (first_hsync_loc - estimated_hsync_loc) / input.meanlinelen;
            result.prev_hsync_diff = prev_hsync_diff;
        }

        double hsync_offset = 0.0;
        int hsync_count = 0;
        for (const auto& pulse : input.validpulses) {
            if (pulse.type != 0 || !pulse.valid) {
                continue;
            }
            const double lineloc =
                (pulse.start - first_hsync_loc) / input.meanlinelen + hsync_start_line;
            const int rlineloc = round_to_int(lineloc);
            if (rlineloc > current_field_lines) {
                break;
            }
            if (rlineloc >= hsync_start_line) {
                hsync_offset +=
                    first_hsync_loc +
                    input.meanlinelen * (rlineloc - hsync_start_line) -
                    pulse.start;
                hsync_count += 1;
            }
        }
        if (hsync_count > 0) {
            hsync_offset /= hsync_count;
            first_hsync_loc -= hsync_offset;
        }

        result.has_first_hsync_loc = true;
        result.first_hsync_loc = first_hsync_loc;
        result.has_line0loc = true;
        result.line0loc = first_hsync_loc - input.meanlinelen * hsync_start_line;
        result.has_next_field = true;
        result.next_field = first_hsync_loc +
            input.meanlinelen * (vblank_lines[LAST_VBLANK_EQ_1_START] - hsync_start_line);
    }

    return result;
}

ValidPulsesToLinelocsResult valid_pulses_to_linelocs(
    const std::vector<double>& valid_pulses_in,
    int reference_pulse,
    int reference_line,
    double meanlinelen,
    double /*hsync_tolerance*/,
    int proclines,
    double /*gap_detection_threshold*/) {
    ValidPulsesToLinelocsResult result;
    result.line_locations.resize(static_cast<std::size_t>(proclines));
    result.line_location_errs.assign(static_cast<std::size_t>(proclines), 0);

    std::vector<double> valid_pulses = valid_pulses_in;
    std::sort(valid_pulses.begin(), valid_pulses.end());

    std::size_t current_pulse_index = 0;
    const std::size_t validpulses_len = valid_pulses.size();
    const double max_allowed_distance_between_pulse_and_line = meanlinelen / 1.5;
    double current_pulse_sample_location = -1.0;

    for (int line_index = 0; line_index < proclines; ++line_index) {
        result.line_locations[static_cast<std::size_t>(line_index)] =
            static_cast<double>(reference_pulse) +
            meanlinelen * static_cast<double>(line_index - reference_line);

        if (current_pulse_index < validpulses_len) {
            double current_distance_from_pulse_to_line = c_abs(
                valid_pulses[current_pulse_index] -
                result.line_locations[static_cast<std::size_t>(line_index)]);
            double smallest_distance_observed_from_pulse_to_line =
                max_allowed_distance_between_pulse_and_line;
            double next_observed_distance_between_pulse_and_line = -1.0;
            current_pulse_sample_location = -1.0;
            std::size_t pulse_search_index = current_pulse_index;

            while (pulse_search_index < validpulses_len - 1) {
                if (current_distance_from_pulse_to_line <=
                    smallest_distance_observed_from_pulse_to_line) {
                    smallest_distance_observed_from_pulse_to_line =
                        current_distance_from_pulse_to_line;
                    current_pulse_index = pulse_search_index;
                    current_pulse_sample_location = valid_pulses[pulse_search_index];
                }

                next_observed_distance_between_pulse_and_line = c_abs(
                    valid_pulses[pulse_search_index + 1] -
                    result.line_locations[static_cast<std::size_t>(line_index)]);
                if (next_observed_distance_between_pulse_and_line >
                    current_distance_from_pulse_to_line) {
                    break;
                }

                current_distance_from_pulse_to_line =
                    next_observed_distance_between_pulse_and_line;
                ++pulse_search_index;
            }

            if (current_pulse_sample_location != -1.0) {
                result.line_locations[static_cast<std::size_t>(line_index)] =
                    current_pulse_sample_location;
                ++current_pulse_index;
            }
        }
    }

    result.last_valid_pulse_location = current_pulse_sample_location;
    return result;
}

std::vector<double> refine_linelocs_hsync(
    const HsyncRefineInput& input,
    std::vector<std::uint8_t>& linebad) {
    if (input.linelocs1.size() != input.demod_05.size() && input.demod_05.empty()) {
        throw std::invalid_argument("refine_linelocs_hsync requires demod_05 data");
    }
    if (linebad.size() < input.linelocs1.size()) {
        linebad.resize(input.linelocs1.size(), 0);
    }

    const auto& linelocs_original = input.linelocs1;
    std::vector<double> linelocs_refined = linelocs_original;

    const auto& demod_05 = input.demod_05;
    const int normal_hsync_length = input.normal_hsync_length;
    const int one_usec = input.one_usec;
    const double sample_rate_mhz = input.sample_rate_mhz;
    const bool is_pal = input.is_pal;
    const bool disable_right_hsync = input.disable_right_hsync;
    const double zc_threshold = input.hsync_threshold;
    const double ire_30 = input.ire_30;
    const double ire_n_65 = input.ire_n_65;
    const double ire_110 = input.ire_110;

    bool right_cross_refined = false;
    double refined_from_right_lineloc = -1.0;
    double zc_fr = 0.0;
    double porch_level = 0.0;
    double prev_porch_level = -1.0;
    double sync_level = 0.0;
    int ll1 = 0;
    double zc = 0.0;
    double zc2 = 0.0;
    double right_cross = 0.0;

    for (std::size_t i = 0; i < linelocs_original.size(); ++i) {
        if (inrange(static_cast<double>(i), 3.0, 6.0) ||
            (is_pal && inrange(static_cast<double>(i), 1.0, 2.0))) {
            linebad[i] = 1;
            continue;
        }

        ll1 = round_to_int(linelocs_original[i]) - one_usec;
        zc = calczc_do(demod_05, ll1, zc_threshold, one_usec * 2);

        right_cross = kNoneDouble;
        if (!disable_right_hsync) {
            right_cross = calczc_do(
                demod_05,
                ll1 + normal_hsync_length - one_usec,
                zc_threshold,
                round_to_int(normal_hsync_length) * 2,
                1);
        }
        right_cross_refined = false;

        if (!is_none_double(zc) && !linebad[i]) {
            linelocs_refined[i] = zc;

            const std::size_t hsync_begin =
                clamp_index(round_to_int(zc - (one_usec * 0.75)), demod_05.size());
            const std::size_t hsync_end =
                clamp_index(round_to_int(zc + (one_usec * 3.5)), demod_05.size());

            if (is_out_of_range(demod_05, hsync_begin, hsync_end, ire_n_65, ire_110)) {
                linebad[i] = 1;
                linelocs_refined[i] = linelocs_original[i];
            } else {
                if (c_max(demod_05, hsync_begin, hsync_end) < ire_30) {
                    porch_level = c_median_mean_proxy(
                        demod_05,
                        clamp_index(round_to_int(zc + (one_usec * 8)), demod_05.size()),
                        clamp_index(round_to_int(zc + (one_usec * 9)), demod_05.size()));
                } else if (prev_porch_level > 0.0) {
                    porch_level = prev_porch_level;
                } else {
                    porch_level = c_median_mean_proxy(
                        demod_05,
                        clamp_index(round_to_int(zc - (one_usec * 1.0)), demod_05.size()),
                        clamp_index(round_to_int(zc - (one_usec * 0.5)), demod_05.size()));
                }

                sync_level = c_median_mean_proxy(
                    demod_05,
                    clamp_index(round_to_int(zc + (one_usec * 1)), demod_05.size()),
                    clamp_index(round_to_int(zc + (one_usec * 2.5)), demod_05.size()));

                zc2 = calczc_do(
                    demod_05,
                    ll1,
                    (porch_level + sync_level) / 2.0,
                    400);

                if (!is_none_double(zc2) && c_abs(zc2 - zc) < (one_usec / 2.0)) {
                    linelocs_refined[i] = zc2;
                    prev_porch_level = porch_level;
                } else if (prev_porch_level > 0.0) {
                    zc2 = calczc_do(
                        demod_05,
                        ll1,
                        (prev_porch_level + sync_level) / 2.0,
                        400);
                    if (!is_none_double(zc2) && c_abs(zc2 - zc) < (one_usec / 2.0)) {
                        linelocs_refined[i] = zc2;
                    } else {
                        linebad[i] = 1;
                    }
                } else {
                    linebad[i] = 1;
                }
            }
        } else {
            linebad[i] = 1;
        }

        if (!is_none_double(right_cross)) {
            zc2 = kNoneDouble;
            zc_fr = right_cross - normal_hsync_length;

            const std::size_t hsync_begin =
                clamp_index(round_to_int(zc_fr - (one_usec * 0.75)), demod_05.size());
            const std::size_t hsync_end =
                clamp_index(round_to_int(zc_fr + (one_usec * 8)), demod_05.size());

            if (!is_out_of_range(demod_05, hsync_begin, hsync_end, ire_n_65, ire_30)) {
                porch_level = c_median_mean_proxy(
                    demod_05,
                    clamp_index(
                        round_to_int(zc_fr + normal_hsync_length + (one_usec * 1)),
                        demod_05.size()),
                    clamp_index(
                        round_to_int(zc_fr + normal_hsync_length + (one_usec * 2)),
                        demod_05.size()));

                sync_level = c_median_mean_proxy(
                    demod_05,
                    clamp_index(round_to_int(zc_fr + (one_usec * 1)), demod_05.size()),
                    clamp_index(round_to_int(zc_fr + (one_usec * 2.5)), demod_05.size()));

                zc2 = calczc_do(
                    demod_05,
                    ll1 + normal_hsync_length - one_usec,
                    (porch_level + sync_level) / 2.0,
                    400);

                if (!is_none_double(zc2) && c_abs(zc2 - right_cross) < (one_usec / 2.0)) {
                    // Literal port note: this 2.25*(sample_rate/40) bias is a magic-value
                    // carry-over from sync.pyx and is intentionally preserved for parity.
                    refined_from_right_lineloc =
                        right_cross - normal_hsync_length +
                        (2.25 * (sample_rate_mhz / 40.0));
                    if (c_abs(refined_from_right_lineloc - linelocs_refined[i]) <
                        (one_usec * 2)) {
                        right_cross = zc2;
                        right_cross_refined = true;
                        prev_porch_level = porch_level;
                    }
                }
            }
        }

        if (linebad[i]) {
            linelocs_refined[i] = linelocs_original[i];
        }

        if (!is_none_double(right_cross) && right_cross_refined) {
            linebad[i] = 0;
            linelocs_refined[i] = refined_from_right_lineloc;
        }
    }

    return linelocs_refined;
}

}  // namespace vhsdecode_cpp

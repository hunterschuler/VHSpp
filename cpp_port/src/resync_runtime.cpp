#include "vhsdecode_cpp/resync_runtime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace vhsdecode_cpp {
namespace {

inline bool inrange(double value, double mi, double ma) {
    return value >= mi && value <= ma;
}

double usectoinpx(double freq_mhz, double us) {
    return freq_mhz * us;
}

double median_or_nan(const std::vector<double>& values) {
    if (values.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t mid = sorted.size() / 2;
    if ((sorted.size() & 1U) != 0U) {
        return sorted[mid];
    }
    return 0.5 * (sorted[mid - 1] + sorted[mid]);
}

}  // namespace

ResyncRuntime::ResyncRuntime(const ResyncRuntimeConfig& config)
    : config_(config),
      vsync_serration_(
          vhsdecode::cppport::VsyncSerrationConfig{
              config.sample_rate_hz,
              config.divisor,
              false,
              {config.fps, config.frame_lines, config.eq_pulse_us},
          }),
      field_state_(config.fps) {
    eq_pulselen_ = vsync_serration_.get_eq_pulselen();
    linelen_ = vsync_serration_.get_line_len();
    long_pulse_max_ = linelen_ * 5;
    last_pulse_threshold_ =
        findpulses_range(config_.sysparams_const, config_.sysparams_const.vsync_hz).pulse_hz_max;
}

bool ResyncRuntime::level_check(
    double sync,
    double blank,
    const std::vector<double>& sync_reference,
    bool full) const {
    return check_levels(
        sync_reference,
        config_.sysparams_const.vsync_hz,
        sync,
        blank,
        config_.sysparams_const.vsync_hz,
        config_.sysparams_const.hz_ire,
        full);
}

std::pair<double, double> ResyncRuntime::pulses_levels(
    const ResyncFieldInput& field,
    const std::vector<Pulse>& pulses,
    double pulse_level,
    bool store_in_field_state) {
    const double vsync_len_px = usectoinpx(config_.sample_rate_mhz, config_.vsync_pulse_us);
    const int min_len = static_cast<int>(vsync_len_px * 0.8);
    const int max_len = static_cast<int>(vsync_len_px * 1.2);

    const auto [vsync_locs, vsync_means] = fallback_vsync_loc_means(
        field.demod_05,
        pulses,
        config_.sample_rate_mhz,
        min_len,
        max_len);

    double synclevel = std::numeric_limits<double>::quiet_NaN();
    if (vsync_means.empty()) {
        const auto pulled = field_state_.pull_sync_level();
        if (!pulled.has_value()) {
            return {std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN()};
        }
        synclevel = *pulled;
    } else {
        synclevel = median_or_nan(vsync_means);
        field_state_.set_sync_level(synclevel);
        field_state_.set_locs(vsync_locs);
    }

    const auto black_means =
        pulses_blacklevel(field.demod_05, config_.sample_rate_mhz, pulses, vsync_locs, synclevel);
    double blacklevel = median_or_nan(black_means);
    if (blacklevel < synclevel) {
        blacklevel = std::numeric_limits<double>::quiet_NaN();
    }

    if (std::isnan(blacklevel) || std::isnan(synclevel)) {
        const auto [sl, bl] = field_state_.pull_levels();
        if (!sl.has_value() || !bl.has_value()) {
            return {std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN()};
        }
        synclevel = *sl;
        blacklevel = *bl;
    } else {
        if (level_check(synclevel, blacklevel, field.demod_05, true) &&
            vsync_means.size() > 3U) {
            if (store_in_field_state) {
                field_state_.set_levels(synclevel, blacklevel);
            }
        } else {
            (void)pulse_level;
            return {std::numeric_limits<double>::quiet_NaN(),
                    std::numeric_limits<double>::quiet_NaN()};
        }
    }

    return {synclevel, blacklevel};
}

void ResyncRuntime::add_pulselevels_to_serration_measures(
    const ResyncFieldInput& field,
    const std::vector<double>& demod_05,
    bool check_long) {
    std::optional<double> sync;
    std::optional<double> blank;

    if (vsync_serration_.has_serration()) {
        const auto levels = vsync_serration_.pull_levels();
        if (levels.has_value()) {
            sync = levels->first;
            blank = levels->second;
        }
    } else {
        constexpr double ire_step = 5.0;
        const double min_vsync_check =
            usectoinpx(config_.sample_rate_mhz, config_.vsync_pulse_us) * 0.8;
        const double long_pulse_min =
            usectoinpx(config_.sample_rate_mhz, config_.vsync_pulse_us) * 2.6;
        const double long_pulse_max = static_cast<double>(long_pulse_max_);

        double min_sync = *std::min_element(demod_05.begin(), demod_05.end());
        int retries = 30;
        int num_assumed_vsyncs_prev = 0;
        int long_pulses_prev = 0;
        double prev_min_sync = min_sync;
        bool found_candidate = false;
        bool check_next = true;
        std::vector<int> pulses_starts;
        std::vector<int> pulses_lengths;

        while (retries-- > 0) {
            const auto range = findpulses_range(config_.sysparams_const, min_sync);
            findpulses_numba_raw_reduced(
                demod_05,
                range.pulse_hz_max,
                config_.divisor,
                static_cast<double>(eq_pulselen_) * 1.0 / 8.0,
                static_cast<double>(long_pulse_max_),
                pulses_starts,
                pulses_lengths);

            if (pulses_lengths.size() > 200U) {
                int num_assumed_vsyncs = 0;
                for (int len : pulses_lengths) {
                    if (len > min_vsync_check) {
                        ++num_assumed_vsyncs;
                    }
                }

                int long_pulses = 0;
                if (check_long && num_assumed_vsyncs <= 2) {
                    for (int len : pulses_lengths) {
                        if (inrange(len, long_pulse_min, long_pulse_max)) {
                            ++long_pulses;
                        }
                    }
                }

                if (num_assumed_vsyncs > 4 || long_pulses >= 1) {
                    if ((num_assumed_vsyncs == 12 || long_pulses == 2) && !check_next) {
                        break;
                    } else if (!found_candidate ||
                               num_assumed_vsyncs > num_assumed_vsyncs_prev ||
                               long_pulses > long_pulses_prev) {
                        found_candidate = true;
                        num_assumed_vsyncs_prev = num_assumed_vsyncs;
                        long_pulses_prev = long_pulses;
                        prev_min_sync = min_sync;
                        check_next = true;
                    } else if (num_assumed_vsyncs < num_assumed_vsyncs_prev ||
                               long_pulses < long_pulses_prev ||
                               !check_next) {
                        min_sync = prev_min_sync;
                        const auto rerange =
                            findpulses_range(config_.sysparams_const, min_sync);
                        findpulses_numba_raw(
                            demod_05,
                            rerange.pulse_hz_max,
                            static_cast<double>(eq_pulselen_) * 1.0 / 8.0,
                            static_cast<double>(long_pulse_max_),
                            pulses_starts,
                            pulses_lengths);
                        break;
                    } else {
                        check_next = false;
                    }
                }
            }

            min_sync = iretohz(
                config_.sysparams_const,
                hztoire(config_.sysparams_const, min_sync) + ire_step);
        }

        std::vector<Pulse> pulses;
        pulses.reserve(pulses_starts.size());
        for (std::size_t i = 0; i < pulses_starts.size(); ++i) {
            pulses.push_back(Pulse{pulses_starts[i], pulses_lengths[i]});
        }

        const auto measured =
            pulses_levels(field, pulses, last_pulse_threshold_, true);
        if (std::isnan(measured.first) || std::isnan(measured.second)) {
            return;
        }
        sync = measured.first;
        blank = measured.second;
    }

    if (!sync.has_value() || !blank.has_value()) {
        return;
    }

    const auto range = findpulses_range(config_.sysparams_const, *sync, *blank);
    const auto pulses = findpulses_numba(
        demod_05,
        range.pulse_hz_max,
        static_cast<double>(eq_pulselen_) * 1.0 / 8.0,
        static_cast<double>(long_pulse_max_));
    const auto refreshed = pulses_levels(field, pulses, range.pulse_hz_max, true);
    if (!std::isnan(refreshed.first) && !std::isnan(refreshed.second)) {
        vsync_serration_.push_levels({refreshed.first, refreshed.second});
    }
}

std::vector<Pulse> ResyncRuntime::get_pulses_serration(
    ResyncFieldInput& field,
    bool check_levels) {
    std::vector<double>& sync_reference = field.demod_05;
    std::vector<double>& demod_data = field.demod;

    if (check_levels || !field_state_.has_levels()) {
        if (!(field.color_system == "405" || field.color_system == "819")) {
            vsync_serration_.work(sync_reference);
        }
        add_pulselevels_to_serration_measures(field, sync_reference, field.fallback_vsync);
    }

    if (vsync_serration_.has_levels() || field_state_.has_levels()) {
        double sync = std::numeric_limits<double>::quiet_NaN();
        double blank = std::numeric_limits<double>::quiet_NaN();
        if (vsync_serration_.has_levels()) {
            const auto levels = vsync_serration_.pull_levels();
            if (levels.has_value() &&
                level_check(levels->first, levels->second, sync_reference, true)) {
                sync = levels->first;
                blank = levels->second;
            } else {
                const auto [sl, bl] = field_state_.pull_levels();
                if (sl.has_value() && bl.has_value()) {
                    sync = *sl;
                    blank = *bl;
                } else {
                    sync = config_.sysparams_const.ire0;
                    blank = config_.sysparams_const.vsync_hz;
                }
            }
        } else {
            const auto [sl, bl] = field_state_.pull_levels();
            if (sl.has_value() && bl.has_value()) {
                sync = *sl;
                blank = *bl;
            }
        }

        const double dc_offset = config_.sysparams_const.ire0 - blank;
        for (double& v : sync_reference) v += dc_offset;
        if (!field.disable_dc_offset) {
            for (double& v : demod_data) v += dc_offset;
        }
        sync += dc_offset;
        blank += dc_offset;
        last_pulse_threshold_ = findpulses_range(config_.sysparams_const, sync).pulse_hz_max;
    } else {
        const auto range = findpulses_range(config_.sysparams_const, config_.sysparams_const.vsync_hz);
        const double new_sync = vsync_serration_.mean_bias();
        const double new_blank =
            iretohz(config_.sysparams_const, hztoire(config_.sysparams_const, new_sync) / 2.0);
        if (!field.disable_dc_offset &&
            !(range.pulse_hz_min < new_sync && new_sync < config_.sysparams_const.vsync_hz) &&
            level_check(new_sync, new_blank, sync_reference, true)) {
            const double offset = config_.sysparams_const.vsync_hz - new_sync;
            for (double& v : sync_reference) v += offset;
            for (double& v : demod_data) v += offset;
        }
        last_pulse_threshold_ = range.pulse_hz_max;
    }

    return findpulses_numba(
        sync_reference,
        last_pulse_threshold_,
        static_cast<double>(eq_pulselen_) * 1.0 / 8.0,
        static_cast<double>(long_pulse_max_));
}

std::vector<Pulse> ResyncRuntime::get_pulses(ResyncFieldInput& field, bool check_levels) {
    if (use_serration_) {
        return get_pulses_serration(field, check_levels);
    }
    throw std::runtime_error("non-serration get_pulses path not ported");
}

}  // namespace vhsdecode_cpp

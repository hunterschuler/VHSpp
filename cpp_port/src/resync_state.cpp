#include "vhsdecode_cpp/resync_state.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace vhsdecode_cpp {
namespace {

inline bool inrange(double value, double mi, double ma) {
    return value >= mi && value <= ma;
}

}  // namespace

FieldState::StackableMA::StackableMA(int min_watermark_in, int window_average_in)
    : window_average(window_average_in), min_watermark(min_watermark_in) {}

void FieldState::StackableMA::push(double value) { stack.push_back(value); }

std::optional<double> FieldState::StackableMA::pull() const {
    if (stack.empty()) return std::nullopt;
    const std::size_t begin =
        stack.size() >= static_cast<std::size_t>(window_average)
            ? stack.size() - static_cast<std::size_t>(window_average)
            : 0U;
    const double sum =
        std::accumulate(stack.begin() + static_cast<std::ptrdiff_t>(begin), stack.end(), 0.0);
    return sum / static_cast<double>(stack.size() - begin);
}

bool FieldState::StackableMA::has_values() const {
    return static_cast<int>(stack.size()) > min_watermark;
}

FieldState::FieldState(double fps)
    : blanklevels_(0, fps < 60.0 ? static_cast<int>(std::lround((fps * 2.0) / 5.0))
                                 : static_cast<int>(std::lround((fps * 2.0) / 6.0))),
      synclevels_(0, fps < 60.0 ? static_cast<int>(std::lround((fps * 2.0) / 5.0))
                                : static_cast<int>(std::lround((fps * 2.0) / 6.0))) {}

void FieldState::set_sync_level(double level) { synclevels_.push(level); }

void FieldState::set_levels(double sync, double blank) {
    blanklevels_.push(blank);
    set_sync_level(sync);
}

std::optional<double> FieldState::pull_sync_level() const { return synclevels_.pull(); }

std::pair<std::optional<double>, std::optional<double>> FieldState::pull_levels() const {
    return {pull_sync_level(), blanklevels_.pull()};
}

void FieldState::set_locs(const std::vector<int>& locs) { locs_ = locs; }

const std::vector<int>& FieldState::get_locs() const { return locs_; }

bool FieldState::has_levels() const {
    return blanklevels_.has_values() && synclevels_.has_values();
}

bool check_levels(
    const std::vector<double>& data,
    double old_sync,
    double new_sync,
    double new_blank,
    double vsync_hz_ref,
    double hz_ire,
    bool full) {
    const double blank_sync_ire_diff = (new_blank - new_sync) / hz_ire;
    if ((vsync_hz_ref - new_sync) > (hz_ire * 15.0) || blank_sync_ire_diff > 47.0) {
        return false;
    }
    if (new_sync - old_sync < (hz_ire * 5.0)) {
        return true;
    }
    if (full && !data.empty()) {
        int amount_below = 0;
        int amount_below_half_sync = 0;
        for (double v : data) {
            if (v < new_sync) ++amount_below;
            if (v < new_blank) ++amount_below_half_sync;
        }
        const double frac_below = static_cast<double>(amount_below) / static_cast<double>(data.size());
        const double frac_below_half_sync =
            static_cast<double>(amount_below_half_sync) / static_cast<double>(data.size());
        if (frac_below > 0.07 || frac_below_half_sync < 0.005) {
            return false;
        }
    }
    return true;
}

std::pair<std::vector<int>, std::vector<double>> fallback_vsync_loc_means(
    const std::vector<double>& demod_05,
    const std::vector<Pulse>& pulses,
    double sample_freq_mhz,
    int min_len,
    int max_len) {
    const double mean_pos_offset = sample_freq_mhz;
    std::vector<int> vsync_locs;
    std::vector<double> vsync_means;
    for (std::size_t i = 0; i < pulses.size(); ++i) {
        const auto& pulse = pulses[i];
        if (!(pulse.len < max_len && pulse.len > min_len)) continue;
        const int begin = std::max(0, static_cast<int>(pulse.start + mean_pos_offset));
        const int end = std::min(
            static_cast<int>(demod_05.size()),
            static_cast<int>(pulse.start + pulse.len - mean_pos_offset));
        if (end <= begin) continue;
        const double sum = std::accumulate(
            demod_05.begin() + begin, demod_05.begin() + end, 0.0);
        vsync_locs.push_back(static_cast<int>(i));
        vsync_means.push_back(sum / static_cast<double>(end - begin));
    }
    return {vsync_locs, vsync_means};
}

std::vector<double> pulses_blacklevel(
    const std::vector<double>& demod_05,
    double freq_mhz,
    const std::vector<Pulse>& pulses,
    const std::vector<int>& vsync_locs,
    double /*synclevel*/) {
    if (vsync_locs.empty()) return {};
    int before_first = vsync_locs.front();
    int after_last = vsync_locs.back();
    const int last_index = static_cast<int>(pulses.size()) - 1;
    if (vsync_locs.size() != 12U) {
        while (before_first > 1 &&
               pulses[static_cast<std::size_t>(before_first)].start -
                       pulses[static_cast<std::size_t>(before_first - 1)].start <
                   600) {
            before_first -= 1;
        }
        while (after_last < last_index &&
               pulses[static_cast<std::size_t>(after_last)].start -
                       pulses[static_cast<std::size_t>(after_last + 1)].start <
                   600) {
            after_last += 1;
        }
    }
    std::vector<double> black_means;
    auto add_range = [&](int from, int to) {
        for (int i = from; i < to; ++i) {
            if (i < 0 || i >= static_cast<int>(pulses.size())) continue;
            const auto& p = pulses[static_cast<std::size_t>(i)];
            if (inrange(p.len, freq_mhz * 0.75, freq_mhz * 3.0)) {
                const int begin = std::max(0, static_cast<int>(p.start + (freq_mhz * 5.0)));
                const int end = std::min(
                    static_cast<int>(demod_05.size()),
                    static_cast<int>(p.start + (freq_mhz * 20.0)));
                if (end <= begin) continue;
                const double sum =
                    std::accumulate(demod_05.begin() + begin, demod_05.begin() + end, 0.0);
                black_means.push_back(sum / static_cast<double>(end - begin));
            }
        }
    };
    if (before_first > 1) add_range(std::max(before_first - 5, 1), before_first);
    if (after_last < last_index - 1) add_range(after_last + 1, std::max(after_last + 6, last_index));
    return black_means;
}

}  // namespace vhsdecode_cpp

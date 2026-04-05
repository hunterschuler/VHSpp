#pragma once

#include "vhsdecode_cpp/resync_core.h"

#include <optional>
#include <utility>
#include <vector>

namespace vhsdecode_cpp {

struct FieldState {
    explicit FieldState(double fps);

    void set_sync_level(double level);
    void set_levels(double sync, double blank);
    std::optional<double> pull_sync_level() const;
    std::pair<std::optional<double>, std::optional<double>> pull_levels() const;
    void set_locs(const std::vector<int>& locs);
    const std::vector<int>& get_locs() const;
    bool has_levels() const;

private:
    struct StackableMA {
        StackableMA(int min_watermark = 0, int window_average = 30);
        void push(double value);
        std::optional<double> pull() const;
        bool has_values() const;

    private:
        int window_average = 30;
        int min_watermark = 0;
        std::vector<double> stack;
    };

    StackableMA blanklevels_;
    StackableMA synclevels_;
    std::vector<int> locs_;
};

bool check_levels(
    const std::vector<double>& data,
    double old_sync,
    double new_sync,
    double new_blank,
    double vsync_hz_ref,
    double hz_ire,
    bool full = true);

std::pair<std::vector<int>, std::vector<double>> fallback_vsync_loc_means(
    const std::vector<double>& demod_05,
    const std::vector<Pulse>& pulses,
    double sample_freq_mhz,
    int min_len,
    int max_len);

std::vector<double> pulses_blacklevel(
    const std::vector<double>& demod_05,
    double freq_mhz,
    const std::vector<Pulse>& pulses,
    const std::vector<int>& vsync_locs,
    double synclevel);

}  // namespace vhsdecode_cpp

#include "vhsdecode_cpp/resync_core.h"

#include <cmath>
#include <stdexcept>

namespace vhsdecode_cpp {
namespace {

inline bool inrange(double value, double mi, double ma) {
    return value >= mi && value <= ma;
}

}  // namespace

double iretohz(const SysParamsConst& sysparams, double ire) {
    return sysparams.ire0 + (sysparams.hz_ire * ire);
}

double hztoire(const SysParamsConst& sysparams, double hz, double ire0) {
    const double base_ire0 = (ire0 == 0.0) ? sysparams.ire0 : ire0;
    return (hz - base_ire0) / sysparams.hz_ire;
}

PulseRange findpulses_range(
    const SysParamsConst& sysparams,
    double vsync_hz,
    double blank_hz) {
    if (blank_hz == 0.0) {
        blank_hz = iretohz(sysparams, 0.0);
    }
    const double sync_ire = hztoire(sysparams, vsync_hz);
    PulseRange range{};
    range.pulse_hz_min = iretohz(sysparams, sync_ire - 10.0);
    range.pulse_hz_max = (iretohz(sysparams, sync_ire) + blank_hz) / 2.0;
    return range;
}

void findpulses_numba_raw(
    const std::vector<double>& sync_ref,
    double high,
    double min_synclen,
    double max_synclen,
    std::vector<int>& starts,
    std::vector<int>& lengths) {
    starts.clear();
    lengths.clear();
    if (sync_ref.empty()) {
        return;
    }

    bool in_pulse = sync_ref.front() <= high;
    int cur_start = 0;

    for (std::size_t pos = 0; pos < sync_ref.size(); ++pos) {
        const double value = sync_ref[pos];
        if (in_pulse) {
            if (value > high) {
                const int length = static_cast<int>(pos) - cur_start;
                if (inrange(length, min_synclen, max_synclen) && cur_start != 0) {
                    starts.push_back(cur_start);
                    lengths.push_back(length);
                }
                in_pulse = false;
            }
        } else if (value <= high) {
            cur_start = static_cast<int>(pos);
            in_pulse = true;
        }
    }
}

std::vector<Pulse> findpulses_numba(
    const std::vector<double>& sync_ref,
    double high,
    double min_synclen,
    double max_synclen) {
    std::vector<int> starts;
    std::vector<int> lengths;
    findpulses_numba_raw(sync_ref, high, min_synclen, max_synclen, starts, lengths);
    std::vector<Pulse> pulses;
    pulses.reserve(starts.size());
    for (std::size_t i = 0; i < starts.size(); ++i) {
        pulses.push_back(Pulse{starts[i], lengths[i]});
    }
    return pulses;
}

void findpulses_numba_raw_reduced(
    const std::vector<double>& sync_ref,
    double high,
    int divisor,
    double min_synclen,
    double max_synclen,
    std::vector<int>& starts,
    std::vector<int>& lengths) {
    if (divisor <= 0) {
        throw std::invalid_argument("divisor must be positive");
    }
    std::vector<double> reduced;
    reduced.reserve((sync_ref.size() + static_cast<std::size_t>(divisor) - 1U) /
                    static_cast<std::size_t>(divisor));
    for (std::size_t i = 0; i < sync_ref.size(); i += static_cast<std::size_t>(divisor)) {
        reduced.push_back(sync_ref[i]);
    }

    findpulses_numba_raw(
        reduced,
        high,
        min_synclen / divisor,
        max_synclen / divisor,
        starts,
        lengths);

    for (auto& start : starts) {
        start *= divisor;
    }
    for (auto& length : lengths) {
        length *= divisor;
    }
}

}  // namespace vhsdecode_cpp

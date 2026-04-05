#pragma once

#include <vector>

namespace vhsdecode_cpp {

struct Pulse {
    int start = 0;
    int len = 0;
};

struct SysParamsConst {
    double ire0 = 0.0;
    double hz_ire = 0.0;
    double vsync_hz = 0.0;
};

double iretohz(const SysParamsConst& sysparams, double ire);
double hztoire(const SysParamsConst& sysparams, double hz, double ire0 = 0.0);

struct PulseRange {
    double pulse_hz_min = 0.0;
    double pulse_hz_max = 0.0;
};

PulseRange findpulses_range(
    const SysParamsConst& sysparams,
    double vsync_hz,
    double blank_hz = 0.0);

std::vector<Pulse> findpulses_numba(
    const std::vector<double>& sync_ref,
    double high,
    double min_synclen,
    double max_synclen);

void findpulses_numba_raw(
    const std::vector<double>& sync_ref,
    double high,
    double min_synclen,
    double max_synclen,
    std::vector<int>& starts,
    std::vector<int>& lengths);

void findpulses_numba_raw_reduced(
    const std::vector<double>& sync_ref,
    double high,
    int divisor,
    double min_synclen,
    double max_synclen,
    std::vector<int>& starts,
    std::vector<int>& lengths);

}  // namespace vhsdecode_cpp

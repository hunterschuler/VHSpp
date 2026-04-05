#include "vhsdecode_cpp/chroma_phase.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace vhsdecode_cpp {
namespace {

using vhsdecode::cppport::SosFilter;
using vhsdecode::cppport::SosSection;

constexpr double kPi = 3.14159265358979323846;

void require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

std::vector<double> odd_ext(const std::vector<double>& x, std::size_t edge) {
    if (edge == 0) return x;
    require(x.size() > edge, "odd_ext requires edge < len(x)");
    std::vector<double> ext;
    ext.reserve(x.size() + 2 * edge);
    const double left = x.front();
    const double right = x.back();
    for (std::size_t i = 0; i < edge; ++i) {
        ext.push_back((2.0 * left) - x[edge - i]);
    }
    ext.insert(ext.end(), x.begin(), x.end());
    for (std::size_t i = 0; i < edge; ++i) {
        ext.push_back((2.0 * right) - x[x.size() - 2 - i]);
    }
    return ext;
}

std::vector<double> solve_linear_system(std::vector<double> a, std::vector<double> b, int n) {
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        double best = std::abs(a[static_cast<std::size_t>(col) * n + col]);
        for (int row = col + 1; row < n; ++row) {
            const double cand = std::abs(a[static_cast<std::size_t>(row) * n + col]);
            if (cand > best) {
                best = cand;
                pivot = row;
            }
        }
        require(best > 1e-18, "singular linear system");
        if (pivot != col) {
            for (int k = col; k < n; ++k) {
                std::swap(a[static_cast<std::size_t>(col) * n + k],
                          a[static_cast<std::size_t>(pivot) * n + k]);
            }
            std::swap(b[static_cast<std::size_t>(col)], b[static_cast<std::size_t>(pivot)]);
        }
        const double diag = a[static_cast<std::size_t>(col) * n + col];
        for (int k = col; k < n; ++k) {
            a[static_cast<std::size_t>(col) * n + k] /= diag;
        }
        b[static_cast<std::size_t>(col)] /= diag;
        for (int row = 0; row < n; ++row) {
            if (row == col) continue;
            const double factor = a[static_cast<std::size_t>(row) * n + col];
            if (factor == 0.0) continue;
            for (int k = col; k < n; ++k) {
                a[static_cast<std::size_t>(row) * n + k] -=
                    factor * a[static_cast<std::size_t>(col) * n + k];
            }
            b[static_cast<std::size_t>(row)] -= factor * b[static_cast<std::size_t>(col)];
        }
    }
    return b;
}

std::vector<double> lfilter_zi_cpp(const std::vector<double>& b_in, const std::vector<double>& a_in) {
    require(!a_in.empty() && !b_in.empty(), "iir filter coefficients are empty");
    std::vector<double> a = a_in;
    std::vector<double> b = b_in;
    while (a.size() > 1 && a.front() == 0.0) a.erase(a.begin());
    require(!a.empty(), "no nonzero a coeffs");
    if (a.front() != 1.0) {
        const double a0 = a.front();
        for (double& v : a) v /= a0;
        for (double& v : b) v /= a0;
    }
    const std::size_t n = std::max(a.size(), b.size());
    a.resize(n, 0.0);
    b.resize(n, 0.0);
    if (n <= 1) return {};
    const int order = static_cast<int>(n - 1);
    std::vector<double> iminus_a(static_cast<std::size_t>(order * order), 0.0);
    for (int row = 0; row < order; ++row) {
        for (int col = 0; col < order; ++col) {
            double aval = 0.0;
            if (col == 0) {
                aval = -a[static_cast<std::size_t>(row + 1)];
            } else if (row == col - 1) {
                aval = 1.0;
            }
            const double eye = (row == col) ? 1.0 : 0.0;
            iminus_a[static_cast<std::size_t>(row) * order + col] = eye - aval;
        }
    }
    std::vector<double> B(order);
    for (int i = 0; i < order; ++i) {
        B[static_cast<std::size_t>(i)] =
            b[static_cast<std::size_t>(i + 1)] - a[static_cast<std::size_t>(i + 1)] * b[0];
    }
    return solve_linear_system(std::move(iminus_a), std::move(B), order);
}

std::array<double, 2> sos_section_zi_cpp(const SosSection& sec) {
    auto zi = lfilter_zi_cpp({sec.b0, sec.b1, sec.b2}, {sec.a0, sec.a1, sec.a2});
    require(zi.size() == 2, "unexpected SOS zi length");
    return {zi[0], zi[1]};
}

std::vector<double> sosfiltfilt_cpp(const SosFilter& filter, const std::vector<double>& input) {
    const std::size_t n_sections = filter.sections.size();
    const std::size_t zeros_b2 = std::count_if(filter.sections.begin(), filter.sections.end(),
                                               [](const SosSection& sec) { return sec.b2 == 0.0; });
    const std::size_t zeros_a2 = std::count_if(filter.sections.begin(), filter.sections.end(),
                                               [](const SosSection& sec) { return sec.a2 == 0.0; });
    std::size_t ntaps = (2 * n_sections) + 1;
    ntaps -= std::min(zeros_b2, zeros_a2);
    if (input.size() <= 1) return input;
    std::size_t edge = std::min<std::size_t>(3 * ntaps, input.size() - 1);
    const std::vector<double> ext = odd_ext(input, edge);
    std::vector<double> y = ext;
    double scale = 1.0;
    const double forward_x0 = y.front();
    for (const auto& sec : filter.sections) {
        const auto zi_base = sos_section_zi_cpp(sec);
        std::array<double, 2> zi = {zi_base[0] * forward_x0 * scale, zi_base[1] * forward_x0 * scale};
        const double a0 = (sec.a0 == 0.0) ? 1.0 : sec.a0;
        const double b0 = sec.b0 / a0, b1 = sec.b1 / a0, b2 = sec.b2 / a0;
        const double a1 = sec.a1 / a0, a2 = sec.a2 / a0;
        for (double& x : y) {
            const double outv = b0 * x + zi[0];
            zi[0] = b1 * x - a1 * outv + zi[1];
            zi[1] = b2 * x - a2 * outv;
            x = outv;
        }
        scale *= (b0 + b1 + b2) / (1.0 + a1 + a2);
    }
    std::reverse(y.begin(), y.end());
    scale = 1.0;
    const double reverse_x0 = y.front();
    for (const auto& sec : filter.sections) {
        const auto zi_base = sos_section_zi_cpp(sec);
        std::array<double, 2> zi = {zi_base[0] * reverse_x0 * scale, zi_base[1] * reverse_x0 * scale};
        const double a0 = (sec.a0 == 0.0) ? 1.0 : sec.a0;
        const double b0 = sec.b0 / a0, b1 = sec.b1 / a0, b2 = sec.b2 / a0;
        const double a1 = sec.a1 / a0, a2 = sec.a2 / a0;
        for (double& x : y) {
            const double outv = b0 * x + zi[0];
            zi[0] = b1 * x - a1 * outv + zi[1];
            zi[1] = b2 * x - a2 * outv;
            x = outv;
        }
        scale *= (b0 + b1 + b2) / (1.0 + a1 + a2);
    }
    std::reverse(y.begin(), y.end());
    if (edge == 0) return y;
    return std::vector<double>(y.begin() + static_cast<std::ptrdiff_t>(edge),
                               y.end() - static_cast<std::ptrdiff_t>(edge));
}

struct BurstResult {
    double phase_deg = 0.0;
    double magnitude = 0.0;
    double i = 0.0;
    double q = 0.0;
};

BurstResult demod_burst(const std::vector<double>& burst,
                        int burst_start,
                        int burst_len,
                        const std::vector<double>& burst_sin,
                        const std::vector<double>& burst_cos) {
    double i_acc = 0.0;
    double q_acc = 0.0;
    for (int i = 0; i < burst_len; ++i) {
        i_acc += burst[static_cast<std::size_t>(i)] * burst_cos[static_cast<std::size_t>(i + burst_start)];
        q_acc += burst[static_cast<std::size_t>(i)] * burst_sin[static_cast<std::size_t>(i + burst_start)];
    }
    const double magnitude = std::hypot(i_acc, q_acc);
    const double phase = std::atan2(q_acc, i_acc);
    double phase_deg = std::fmod((phase * 180.0 / kPi), 360.0);
    if (phase_deg < 0.0) phase_deg += 360.0;
    return {phase_deg, magnitude, i_acc, q_acc};
}

BurstResult get_upconverted_burst(const ChromaPhaseInput& input,
                                  int current_phase,
                                  int linenumber) {
    const int burst_padding = input.burst_end - input.burst_start;
    const int line_start = (linenumber - input.lineoffset) * input.outwidth;
    const int raw_start = std::max(0, line_start + input.burst_start - burst_padding);
    const int raw_end = std::min(static_cast<int>(input.chroma.size()), raw_start + input.burst_end + burst_padding);
    std::vector<double> burst(static_cast<std::size_t>(std::max(0, raw_end - raw_start)), 0.0);
    const auto& het = input.chroma_heterodyne[static_cast<std::size_t>(current_phase)];
    for (int idx = raw_start; idx < raw_end; ++idx) {
        burst[static_cast<std::size_t>(idx - raw_start)] = het[static_cast<std::size_t>(idx)] * input.chroma[static_cast<std::size_t>(idx)];
    }
    std::vector<double> filtered_padded = sosfiltfilt_cpp(input.chroma_filter, burst);
    std::vector<double> filtered;
    if (filtered_padded.size() > static_cast<std::size_t>(2 * burst_padding)) {
        filtered.assign(filtered_padded.begin() + burst_padding, filtered_padded.end() - burst_padding);
    }
    const int burst_len = static_cast<int>(filtered.size());
    return demod_burst(filtered, raw_start + burst_padding, burst_len, input.burst_sin, input.burst_cos);
}

int wrap_phase_mod4(int value) {
    value %= 4;
    if (value < 0) value += 4;
    return value;
}

std::pair<int, std::vector<PhaseSequenceEntry>> get_phase_sequence_inner(const ChromaPhaseInput& input,
                                                                         int chroma_rotation_starting_index) {
    const bool do_phase_rotation_check =
        input.detect_chroma_track_phase && input.chroma_rotation.has_value() && !input.chroma_heterodyne.empty();
    std::vector<PhaseSequenceEntry> phase_sequence;
    int chroma_rotation_index = chroma_rotation_starting_index;
    if (!input.chroma_rotation_index.has_value()) {
        chroma_rotation_index = 0;
    }
    int track_rotation = 0;
    if (input.chroma_rotation.has_value()) {
        chroma_rotation_index = chroma_rotation_starting_index;
        track_rotation = (*input.chroma_rotation)[static_cast<std::size_t>(chroma_rotation_index)];
    } else {
        chroma_rotation_index = 0;
        track_rotation = chroma_rotation_starting_index;
    }

    const int last_line = input.linesout + input.lineoffset;
    const double track_change_threshold = 90.0;
    int current_phase = 0;
    bool use_next_phase = false;
    BurstResult next_burst{};
    int next_phase = 0;

    for (int linenumber = input.lineoffset; linenumber < last_line; ++linenumber) {
        BurstResult current_burst{};
        if (use_next_phase) {
            current_phase = next_phase;
            current_burst = next_burst;
            use_next_phase = false;
        } else {
            current_phase = wrap_phase_mod4(current_phase + track_rotation);
            current_burst = get_upconverted_burst(input, current_phase, linenumber);
        }

        if (do_phase_rotation_check &&
            linenumber >= input.rotation_check_start_line &&
            linenumber < last_line - 1) {
            next_phase = wrap_phase_mod4(current_phase + track_rotation);
            next_burst = get_upconverted_burst(input, next_phase, linenumber + 1);
            const double phase_delta_quadrant =
                std::abs(std::fmod((next_burst.phase_deg - current_burst.phase_deg + 180.0), 360.0) - 180.0);
            if (phase_delta_quadrant > track_change_threshold) {
                chroma_rotation_index = (chroma_rotation_index + 1) % 2;
                track_rotation = (*input.chroma_rotation)[static_cast<std::size_t>(chroma_rotation_index)];
            } else {
                use_next_phase = true;
            }
        }

        phase_sequence.push_back({
            linenumber,
            current_phase,
            current_burst.phase_deg,
            current_burst.magnitude,
            current_burst.i,
            current_burst.q,
        });
    }

    if (input.chroma_rotation.has_value() &&
        chroma_rotation_index == chroma_rotation_starting_index) {
        chroma_rotation_index = (chroma_rotation_index + 1) % 2;
    }
    return {chroma_rotation_index, phase_sequence};
}

}  // namespace

ChromaPhaseResult get_phase_rotation_sequence(const ChromaPhaseInput& input) {
    require(static_cast<int>(input.chroma_heterodyne.size()) == 4,
            "phase rotation expects four heterodyne phases");
    require(input.outwidth > 0, "outwidth must be positive");

    int starting_index = input.chroma_rotation_index.value_or(0);
    auto [track_phase, phase_sequence] = get_phase_sequence_inner(input, starting_index);

    const double coherence_threshold = 0.3;
    const int burst_check_skip_lines = 16;
    const int burst_check_start = burst_check_skip_lines;
    const int burst_check_end = input.linesout + input.lineoffset - burst_check_skip_lines;

    bool flip_track_phase = false;
    if (input.chroma_rotation.has_value()) {
        int delta_0 = 0;
        int delta_90 = 0;
        int delta_180 = 0;
        int delta_270 = 0;
        for (std::size_t i = 1; i < phase_sequence.size(); ++i) {
            const auto& prev = phase_sequence[i - 1];
            const auto& curr = phase_sequence[i];
            if (curr.line_number > burst_check_start && curr.line_number < burst_check_end) {
                const double delta = std::fmod(curr.burst_phase - prev.burst_phase + 360.0, 360.0);
                const int bucket = static_cast<int>(std::floor((delta + 45.0) / 90.0)) % 4;
                if (bucket == 0) ++delta_0;
                else if (bucket == 1) ++delta_90;
                else if (bucket == 2) ++delta_180;
                else ++delta_270;
            }
        }
        if (input.is_ntsc) {
            flip_track_phase = delta_0 < delta_180;
        } else {
            const int alt1 = delta_90 + delta_270;
            const int alt2 = delta_0 + delta_180;
            flip_track_phase = alt1 < alt2;
        }
    }

    if (flip_track_phase) {
        std::tie(track_phase, phase_sequence) = get_phase_sequence_inner(input, track_phase);
    }

    ChromaPhaseResult result{};
    result.track_phase = track_phase;
    result.phase_sequence = std::move(phase_sequence);

    if (input.is_ntsc) {
        double i_total = 0.0;
        double q_total = 0.0;
        int avg_count = 0;
        for (const auto& entry : result.phase_sequence) {
            if (entry.line_number > burst_check_start && entry.line_number < burst_check_end) {
                if (entry.burst_magnitude != 0.0) {
                    i_total += entry.burst_i / entry.burst_magnitude;
                    q_total += entry.burst_q / entry.burst_magnitude;
                    ++avg_count;
                }
            }
        }
        if (avg_count > 0) {
            const double coherence = std::hypot(i_total, q_total) / static_cast<double>(avg_count);
            result.burst_detected = coherence >= coherence_threshold;
            double phase_deg = std::atan2(q_total, i_total) * 180.0 / kPi;
            phase_deg = std::fmod(phase_deg, 360.0);
            if (phase_deg < 0.0) phase_deg += 360.0;
            result.burst_phase_avg = phase_deg;
        } else {
            result.burst_detected = false;
            result.burst_phase_avg = 0.0;
        }
    }

    return result;
}

std::vector<double> upconvert_chroma(const std::vector<double>& chroma,
                                     int lineoffset,
                                     int outwidth,
                                     const std::vector<std::vector<double>>& chroma_heterodyne,
                                     const std::vector<PhaseSequenceEntry>& phase_rotation_sequence) {
    std::vector<double> uphet(chroma.size(), 0.0);
    for (const auto& entry : phase_rotation_sequence) {
        const int line_start = (entry.line_number - lineoffset) * outwidth;
        const int line_end = line_start + outwidth;
        const auto& heterodyne = chroma_heterodyne[static_cast<std::size_t>(entry.current_phase)];
        for (int i = line_start; i < line_end; ++i) {
            uphet[static_cast<std::size_t>(i)] =
                chroma[static_cast<std::size_t>(i)] * heterodyne[static_cast<std::size_t>(i)];
        }
    }
    return uphet;
}

}  // namespace vhsdecode_cpp

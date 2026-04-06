#include "vhsdecode_cpp/chroma_process.h"
#include "vhsdecode_cpp/fftw_guard.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

#include <fftw3.h>

namespace vhsdecode_cpp {
namespace {

using vhsdecode::cppport::SosFilter;
using vhsdecode::cppport::SosSection;

constexpr double kPi = 3.14159265358979323846;
constexpr double kS16AbsMax = 32767.0;
constexpr bool kEnableDetailedChromaPerf = false;

struct ComplexFftPlanCache {
    int n = 0;
    fftw_complex* in = nullptr;
    fftw_complex* out = nullptr;
    fftw_plan plan = nullptr;

    ~ComplexFftPlanCache() {
        if (plan) fftw_destroy_plan(plan);
        if (in) fftw_free(in);
        if (out) fftw_free(out);
    }

    void ensure(int wanted_n, int direction) {
        if (plan != nullptr && wanted_n == n) return;
        if (plan) fftw_destroy_plan(plan);
        if (in) fftw_free(in);
        if (out) fftw_free(out);
        n = wanted_n;
        in = reinterpret_cast<fftw_complex*>(
            fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(n)));
        out = reinterpret_cast<fftw_complex*>(
            fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(n)));
        if (!(in && out)) throw std::runtime_error("fftw alloc failed");
        std::lock_guard<std::mutex> lock(vhsdecode::cppport::fftw_plan_mutex());
        plan = fftw_plan_dft_1d(n, in, out, direction, FFTW_ESTIMATE);
        if (plan == nullptr) throw std::runtime_error("fftw plan failed");
    }
};

struct HilbertMaskCache {
    std::size_t n = 0;
    std::vector<double> h;

    void ensure(std::size_t wanted_n) {
        if (wanted_n == n) return;
        n = wanted_n;
        h.assign(n, 0.0);
        if (n == 0) return;
        h[0] = 1.0;
        if ((n % 2U) == 0U) {
            h[n / 2] = 1.0;
            for (std::size_t i = 1; i < n / 2; ++i) h[i] = 2.0;
        } else {
            for (std::size_t i = 1; i <= n / 2; ++i) h[i] = 2.0;
        }
    }
};

std::array<double, 2> sos_section_zi_cpp(const SosSection& sec);

struct SosSectionRuntime {
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double dc_scale = 1.0;
    std::array<double, 2> zi_base = {0.0, 0.0};
};

struct SosRuntimeCache {
    SosFilter filter;
    std::vector<SosSectionRuntime> sections;
    std::size_t ntaps = 0;
    bool valid = false;

    static bool same_filter(const SosFilter& lhs, const SosFilter& rhs) {
        if (lhs.sections.size() != rhs.sections.size()) return false;
        for (std::size_t i = 0; i < lhs.sections.size(); ++i) {
            const auto& a = lhs.sections[i];
            const auto& b = rhs.sections[i];
            if (a.b0 != b.b0 || a.b1 != b.b1 || a.b2 != b.b2 ||
                a.a0 != b.a0 || a.a1 != b.a1 || a.a2 != b.a2) {
                return false;
            }
        }
        return true;
    }

    void ensure(const SosFilter& wanted) {
        if (valid && same_filter(filter, wanted)) return;
        filter = wanted;
        sections.clear();
        sections.reserve(filter.sections.size());
        const std::size_t zeros_b2 = std::count_if(filter.sections.begin(), filter.sections.end(),
                                                   [](const SosSection& sec) { return sec.b2 == 0.0; });
        const std::size_t zeros_a2 = std::count_if(filter.sections.begin(), filter.sections.end(),
                                                   [](const SosSection& sec) { return sec.a2 == 0.0; });
        ntaps = (2 * filter.sections.size()) + 1;
        ntaps -= std::min(zeros_b2, zeros_a2);
        for (const auto& sec : filter.sections) {
            const double a0 = (sec.a0 == 0.0) ? 1.0 : sec.a0;
            SosSectionRuntime rt{};
            rt.b0 = sec.b0 / a0;
            rt.b1 = sec.b1 / a0;
            rt.b2 = sec.b2 / a0;
            rt.a1 = sec.a1 / a0;
            rt.a2 = sec.a2 / a0;
            rt.dc_scale = (rt.b0 + rt.b1 + rt.b2) / (1.0 + rt.a1 + rt.a2);
            rt.zi_base = sos_section_zi_cpp(sec);
            sections.push_back(rt);
        }
        valid = true;
    }
};

void require(bool cond, const char* msg) {
    if (!cond) throw std::runtime_error(msg);
}

std::vector<double> odd_ext(const std::vector<double>& x, std::size_t edge) {
    if (edge == 0) return x;
    require(x.size() > edge, "odd_ext requires edge < len(x)");
    std::vector<double> ext;
    ext.reserve(x.size() + 2 * edge);
    const double left = x.front();
    const double right = x.back();
    for (std::size_t i = 0; i < edge; ++i) ext.push_back((2.0 * left) - x[edge - i]);
    ext.insert(ext.end(), x.begin(), x.end());
    for (std::size_t i = 0; i < edge; ++i) ext.push_back((2.0 * right) - x[x.size() - 2 - i]);
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
                std::swap(a[static_cast<std::size_t>(col) * n + k], a[static_cast<std::size_t>(pivot) * n + k]);
            }
            std::swap(b[static_cast<std::size_t>(col)], b[static_cast<std::size_t>(pivot)]);
        }
        const double diag = a[static_cast<std::size_t>(col) * n + col];
        for (int k = col; k < n; ++k) a[static_cast<std::size_t>(col) * n + k] /= diag;
        b[static_cast<std::size_t>(col)] /= diag;
        for (int row = 0; row < n; ++row) {
            if (row == col) continue;
            const double factor = a[static_cast<std::size_t>(row) * n + col];
            if (factor == 0.0) continue;
            for (int k = col; k < n; ++k) {
                a[static_cast<std::size_t>(row) * n + k] -= factor * a[static_cast<std::size_t>(col) * n + k];
            }
            b[static_cast<std::size_t>(row)] -= factor * b[static_cast<std::size_t>(col)];
        }
    }
    return b;
}

std::vector<double> lfilter_zi_cpp(const vhsdecode::cppport::IirFilter& filter) {
    require(!filter.a.empty() && !filter.b.empty(), "iir filter coefficients are empty");
    std::vector<double> a = filter.a;
    std::vector<double> b = filter.b;
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
            if (col == 0) aval = -a[static_cast<std::size_t>(row + 1)];
            else if (row == col - 1) aval = 1.0;
            const double eye = (row == col) ? 1.0 : 0.0;
            iminus_a[static_cast<std::size_t>(row) * order + col] = eye - aval;
        }
    }
    std::vector<double> rhs(order);
    for (int i = 0; i < order; ++i) {
        rhs[static_cast<std::size_t>(i)] =
            b[static_cast<std::size_t>(i + 1)] - a[static_cast<std::size_t>(i + 1)] * b[0];
    }
    return solve_linear_system(std::move(iminus_a), std::move(rhs), order);
}

std::pair<std::vector<double>, std::vector<double>> iir_lfilter_cpp(
    const vhsdecode::cppport::IirFilter& filter,
    const std::vector<double>& input,
    const std::vector<double>& zi_in) {
    const std::size_t order = std::max(filter.a.size(), filter.b.size()) - 1;
    std::vector<double> a = filter.a;
    std::vector<double> b = filter.b;
    a.resize(order + 1, 0.0);
    b.resize(order + 1, 0.0);
    if (!a.empty() && a[0] != 1.0) {
        const double a0 = a[0];
        for (double& v : a) v /= a0;
        for (double& v : b) v /= a0;
    }
    std::vector<double> z = zi_in;
    z.resize(order, 0.0);
    std::vector<double> out(input.size(), 0.0);
    for (std::size_t n = 0; n < input.size(); ++n) {
        const double x = input[n];
        double y = b[0] * x;
        if (order > 0) y += z[0];
        for (std::size_t i = 1; i < order; ++i) z[i - 1] = z[i] + b[i] * x - a[i] * y;
        if (order > 0) z[order - 1] = b[order] * x - a[order] * y;
        out[n] = y;
    }
    return {out, z};
}

std::vector<double> iir_filtfilt_cpp(const vhsdecode::cppport::IirFilter& filter,
                                     const std::vector<double>& input) {
    if (input.size() <= 1) return input;
    const std::size_t ntaps = std::max(filter.a.size(), filter.b.size());
    const std::size_t edge = std::min<std::size_t>(3 * ntaps, input.size() - 1);
    const std::vector<double> ext = odd_ext(input, edge);
    const std::vector<double> zi_base = lfilter_zi_cpp(filter);
    auto zi = zi_base;
    const double x0 = ext.front();
    for (double& v : zi) v *= x0;
    auto forward = iir_lfilter_cpp(filter, ext, zi).first;
    std::reverse(forward.begin(), forward.end());
    zi = zi_base;
    const double y0 = forward.front();
    for (double& v : zi) v *= y0;
    auto backward = iir_lfilter_cpp(filter, forward, zi).first;
    std::reverse(backward.begin(), backward.end());
    return std::vector<double>(backward.begin() + static_cast<std::ptrdiff_t>(edge),
                               backward.end() - static_cast<std::ptrdiff_t>(edge));
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
            if (col == 0) aval = -a[static_cast<std::size_t>(row + 1)];
            else if (row == col - 1) aval = 1.0;
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
    if (input.size() <= 1) return input;
    thread_local SosRuntimeCache cache;
    cache.ensure(filter);
    std::size_t edge = std::min<std::size_t>(3 * cache.ntaps, input.size() - 1);
    const std::vector<double> ext = odd_ext(input, edge);
    std::vector<double> y = ext;
    double scale = 1.0;
    const double forward_x0 = y.front();
    for (const auto& sec : cache.sections) {
        std::array<double, 2> zi = {sec.zi_base[0] * forward_x0 * scale, sec.zi_base[1] * forward_x0 * scale};
        for (double& x : y) {
            const double outv = sec.b0 * x + zi[0];
            zi[0] = sec.b1 * x - sec.a1 * outv + zi[1];
            zi[1] = sec.b2 * x - sec.a2 * outv;
            x = outv;
        }
        scale *= sec.dc_scale;
    }
    std::reverse(y.begin(), y.end());
    scale = 1.0;
    const double reverse_x0 = y.front();
    for (const auto& sec : cache.sections) {
        std::array<double, 2> zi = {sec.zi_base[0] * reverse_x0 * scale, sec.zi_base[1] * reverse_x0 * scale};
        for (double& x : y) {
            const double outv = sec.b0 * x + zi[0];
            zi[0] = sec.b1 * x - sec.a1 * outv + zi[1];
            zi[1] = sec.b2 * x - sec.a2 * outv;
            x = outv;
        }
        scale *= sec.dc_scale;
    }
    std::reverse(y.begin(), y.end());
    if (edge == 0) return y;
    return std::vector<double>(y.begin() + static_cast<std::ptrdiff_t>(edge),
                               y.end() - static_cast<std::ptrdiff_t>(edge));
}

std::vector<std::complex<double>> fft_complex_full(const std::vector<std::complex<double>>& input) {
    const int n = static_cast<int>(input.size());
    std::vector<std::complex<double>> output(input.size());
    thread_local ComplexFftPlanCache cache;
    cache.ensure(n, FFTW_FORWARD);
    for (int i = 0; i < n; ++i) {
        cache.in[i][0] = input[static_cast<std::size_t>(i)].real();
        cache.in[i][1] = input[static_cast<std::size_t>(i)].imag();
    }
    fftw_execute(cache.plan);
    for (int i = 0; i < n; ++i) output[static_cast<std::size_t>(i)] = {cache.out[i][0], cache.out[i][1]};
    return output;
}

std::vector<std::complex<double>> ifft_complex_full(const std::vector<std::complex<double>>& input) {
    const int n = static_cast<int>(input.size());
    std::vector<std::complex<double>> output(input.size());
    thread_local ComplexFftPlanCache cache;
    cache.ensure(n, FFTW_BACKWARD);
    for (int i = 0; i < n; ++i) {
        cache.in[i][0] = input[static_cast<std::size_t>(i)].real();
        cache.in[i][1] = input[static_cast<std::size_t>(i)].imag();
    }
    fftw_execute(cache.plan);
    const double inv_n = 1.0 / static_cast<double>(n);
    for (int i = 0; i < n; ++i) output[static_cast<std::size_t>(i)] = {cache.out[i][0] * inv_n, cache.out[i][1] * inv_n};
    return output;
}

std::vector<std::complex<double>> analytic_signal_cpp(const std::vector<double>& signal) {
    const std::size_t n = signal.size();
    if (n == 0) return {};
    std::vector<std::complex<double>> real_signal(n);
    for (std::size_t i = 0; i < n; ++i) real_signal[i] = {signal[i], 0.0};
    auto spectrum = fft_complex_full(real_signal);
    thread_local HilbertMaskCache mask_cache;
    mask_cache.ensure(n);
    for (std::size_t i = 0; i < n; ++i) spectrum[i] *= mask_cache.h[i];
    return ifft_complex_full(spectrum);
}

std::vector<double> burst_deemphasis_cpp(std::vector<double> chroma,
                                         int lineoffset,
                                         int linesout,
                                         int outwidth,
                                         int burst_end) {
    for (int line = lineoffset; line < linesout + lineoffset; ++line) {
        const int line_start = (line - lineoffset) * outwidth;
        const int line_end = line_start + outwidth;
        const int start = std::min(line_end, line_start + burst_end + 5);
        for (int i = start; i < line_end; ++i) chroma[static_cast<std::size_t>(i)] *= 2.0;
    }
    return chroma;
}

std::vector<double> shift_chroma_and_remove_dc(std::vector<double> out_chroma, int move) {
    if (!out_chroma.empty()) {
        const int len = static_cast<int>(out_chroma.size());
        const int offset = len == 0 ? 0 : ((move % len) + len) % len;
        if (offset != 0) std::rotate(out_chroma.begin(), out_chroma.end() - offset, out_chroma.end());
        const double avg = std::accumulate(out_chroma.begin(), out_chroma.end(), 0.0) /
                           static_cast<double>(out_chroma.size());
        for (double& v : out_chroma) v -= avg;
    }
    return out_chroma;
}

std::vector<double> demod_chroma_filt_cpp_local(
    const std::vector<double>& data,
    const vhsdecode::cppport::SosFilter& filter,
    std::size_t blocklen,
    const std::optional<vhsdecode::cppport::IirFilter>& notch,
    bool do_notch,
    int move,
    const std::optional<vhsdecode::cppport::IirFilter>& audio_notch) {
    std::vector<double> slice(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(blocklen));
    std::vector<double> out_chroma = sosfiltfilt_cpp(filter, slice);
    if (audio_notch.has_value()) out_chroma = iir_filtfilt_cpp(*audio_notch, out_chroma);
    if (do_notch && notch.has_value()) out_chroma = iir_filtfilt_cpp(*notch, out_chroma);
    return shift_chroma_and_remove_dc(std::move(out_chroma), move);
}

std::vector<double> lfilter_cpp(const vhsdecode::cppport::IirFilter& filter,
                                const std::vector<double>& input) {
    return iir_lfilter_cpp(filter, input, {}).first;
}

std::vector<double> ntsc_phase_comp_cpp(const std::vector<double>& uphet, double burst_phase_avg) {
    auto analytic = analytic_signal_cpp(uphet);
    const double phase_adjustment = -burst_phase_avg * kPi / 180.0;
    const std::complex<double> rotation = std::exp(std::complex<double>(0.0, phase_adjustment));
    std::vector<double> out(uphet.size(), 0.0);
    for (std::size_t i = 0; i < analytic.size(); ++i) out[i] = (analytic[i] * rotation).real();
    return out;
}

std::vector<double> comb_c_ntsc_cpp(std::vector<double> data, int line_len) {
    std::vector<double> data2 = data;
    const int numlines = static_cast<int>(data.size() / static_cast<std::size_t>(line_len));
    for (int line_num = 16; line_num < numlines - 2; ++line_num) {
        const int base = line_num * line_len;
        const int adv = (line_num + 1) * line_len;
        const int del = (line_num - 1) * line_len;
        for (int i = 0; i < line_len; ++i) {
            const double advanced1h = data2[static_cast<std::size_t>(adv + i)];
            const double delayed1h = data2[static_cast<std::size_t>(del + i)];
            const double line_slice = data[static_cast<std::size_t>(base + i)];
            data[static_cast<std::size_t>(base + i)] = ((line_slice * 2.0) - advanced1h - delayed1h) / 4.0;
        }
    }
    return data;
}

double rms_span(const std::vector<double>& v, int start, int end) {
    if (end <= start) return 0.0;
    double acc = 0.0;
    for (int i = start; i < end; ++i) {
        const double x = v[static_cast<std::size_t>(i)];
        acc += x * x;
    }
    return std::sqrt(acc / static_cast<double>(end - start));
}

std::pair<std::vector<double>, double> acc_cpp(const std::vector<double>& chroma,
                                               double burst_abs_ref,
                                               int burststart,
                                               int burstend,
                                               int linelength,
                                               int lines) {
    constexpr int STARTING_LINE = 16;
    require(lines > STARTING_LINE, "acc requires enough lines");
    std::vector<double> output(chroma.size(), 0.0);
    double mean_burst_accumulator = 0.0;
    for (int linenumber = STARTING_LINE; linenumber < lines; ++linenumber) {
        const int linestart = linelength * linenumber;
        const int lineend = linestart + linelength;
        const double burst_abs_mean = rms_span(chroma, linestart + burststart, linestart + burstend);
        const double scale = burst_abs_mean != 0.0 ? burst_abs_ref / burst_abs_mean : 1.0;
        for (int i = linestart; i < lineend; ++i) output[static_cast<std::size_t>(i)] = chroma[static_cast<std::size_t>(i)] * scale;
        mean_burst_accumulator += burst_abs_mean;
    }
    return {output, mean_burst_accumulator / static_cast<double>(lines - STARTING_LINE)};
}

std::vector<std::uint16_t> chroma_to_u16_cpp(const std::vector<double>& chroma) {
    std::vector<std::uint16_t> out(chroma.size());
    for (std::size_t i = 0; i < chroma.size(); ++i) {
        double v = chroma[i] + kS16AbsMax;
        if (v < 0.0) v = 0.0;
        if (v > 65535.0) v = 65535.0;
        out[i] = static_cast<std::uint16_t>(v);
    }
    return out;
}

}  // namespace

ChromaProcessResult process_chroma(const ChromaProcessInput& input) {
    ChromaProcessResult result{};
    // !!! LOUD FORMAT SCOPE NOTE !!!
    // This function now includes the optional CAFC and chroma-deemphasis
    // branches, but the fixture-validated parity lane is still NTSC VHS first.
    // Upstream format coverage is much wider; see LOGBOOK.md before assuming PAL,
    // MPAL, NLINHA, MESECAM, 405/819, Betamax, Video8, U-matic, Type C/B, etc.
    // are validated just because the code compiles here.
    std::vector<double> chroma_after_cafc_prefilter = input.chroma;
    if (input.do_cafc) {
        const auto t0 = kEnableDetailedChromaPerf ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
        require(input.cafc_chroma_bandpass.has_value(), "do_cafc requires cafc_chroma_bandpass");
        chroma_after_cafc_prefilter = demod_chroma_filt_cpp_local(
            chroma_after_cafc_prefilter,
            *input.cafc_chroma_bandpass,
            chroma_after_cafc_prefilter.size(),
            input.fvideo_notch,
            input.enable_video_notch,
            input.cafc_move,
            input.chroma_audio_notch);
        if (!input.disable_tracking_cafc) {
            require(input.chroma_afc != nullptr, "tracking CAFC requires chroma_afc");
            result.cafc_measurement = input.chroma_afc->freqOffset(chroma_after_cafc_prefilter);
        }
        if constexpr (kEnableDetailedChromaPerf) {
            result.perf.cafc_prefilter_s +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
    }
    if (input.keep_intermediates) result.chroma_after_cafc_prefilter = chroma_after_cafc_prefilter;

    std::vector<double> chroma_after_burst_deemph =
        input.keep_intermediates ? chroma_after_cafc_prefilter : std::move(chroma_after_cafc_prefilter);
    if (input.is_ntsc && !input.disable_deemph) {
        const auto t0 = kEnableDetailedChromaPerf ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
        chroma_after_burst_deemph = burst_deemphasis_cpp(
            std::move(chroma_after_burst_deemph),
            input.lineoffset,
            input.linesout,
            input.outwidth,
            input.burst_end);
        if constexpr (kEnableDetailedChromaPerf) {
            result.perf.burst_deemph_s +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
    }
    if (input.keep_intermediates) result.chroma_after_burst_deemph = chroma_after_burst_deemph;

    const std::vector<std::vector<double>>* heterodyne_ptr = input.chroma_heterodyne_ref;
    std::vector<std::vector<double>> heterodyne_owned;
    if (input.do_cafc && !input.disable_tracking_cafc && input.chroma_afc != nullptr) {
        heterodyne_owned = {
            std::vector<double>(input.chroma_afc->getChromaHet()[0].begin(), input.chroma_afc->getChromaHet()[0].end()),
            std::vector<double>(input.chroma_afc->getChromaHet()[1].begin(), input.chroma_afc->getChromaHet()[1].end()),
            std::vector<double>(input.chroma_afc->getChromaHet()[2].begin(), input.chroma_afc->getChromaHet()[2].end()),
            std::vector<double>(input.chroma_afc->getChromaHet()[3].begin(), input.chroma_afc->getChromaHet()[3].end())};
        heterodyne_ptr = &heterodyne_owned;
    } else if (heterodyne_ptr == nullptr) {
        heterodyne_ptr = &input.chroma_heterodyne;
    }
    {
        const auto t0 = kEnableDetailedChromaPerf ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
        std::vector<double> uphet_raw = upconvert_chroma(
            chroma_after_burst_deemph,
            input.lineoffset,
            input.outwidth,
            *heterodyne_ptr,
            input.phase_sequence);
        if (input.keep_intermediates) result.uphet_raw = uphet_raw;
        if constexpr (kEnableDetailedChromaPerf) {
            result.perf.upconvert_s +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        std::vector<double> uphet_phase_comp =
            (input.is_ntsc && !input.disable_phase_correction && input.burst_phase_avg.has_value())
                ? std::vector<double>{}
                : uphet_raw;
        if (input.is_ntsc && !input.disable_phase_correction && input.burst_phase_avg.has_value()) {
            const auto t0 = kEnableDetailedChromaPerf ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
            uphet_phase_comp = ntsc_phase_comp_cpp(uphet_raw, *input.burst_phase_avg);
            if constexpr (kEnableDetailedChromaPerf) {
                result.perf.phase_comp_s +=
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            }
        }
        if (input.keep_intermediates) result.uphet_phase_comp = uphet_phase_comp;

        const auto t1 = kEnableDetailedChromaPerf ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
        std::vector<double> uphet_filtered = sosfiltfilt_cpp(input.fchroma_final, uphet_phase_comp);
        if constexpr (kEnableDetailedChromaPerf) {
            result.perf.final_filter_s +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        }
        if (input.keep_intermediates) result.uphet_filtered = uphet_filtered;

        std::vector<double> uphet_after_chroma_deemph = std::move(uphet_filtered);
        if (input.do_chroma_deemphasis && input.chroma_deemphasis.has_value()) {
            const auto t2 = kEnableDetailedChromaPerf ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
            uphet_after_chroma_deemph = lfilter_cpp(*input.chroma_deemphasis, uphet_after_chroma_deemph);
            if constexpr (kEnableDetailedChromaPerf) {
                result.perf.chroma_deemph_s +=
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t2).count();
            }
        }
        if (input.keep_intermediates) result.uphet_after_chroma_deemph = uphet_after_chroma_deemph;

        std::vector<double> uphet_comb =
            input.disable_comb ? uphet_after_chroma_deemph : std::vector<double>{};
        if (!input.disable_comb) {
            const auto t3 = kEnableDetailedChromaPerf ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
            if (input.is_ntsc) {
                uphet_comb = comb_c_ntsc_cpp(std::move(uphet_after_chroma_deemph), input.outwidth);
            } else {
                uphet_comb = uphet_after_chroma_deemph;
            }
            if constexpr (kEnableDetailedChromaPerf) {
                result.perf.comb_s +=
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t3).count();
            }
        }
        if (input.keep_intermediates) result.uphet_comb = uphet_comb;

        const auto t4 = kEnableDetailedChromaPerf ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
        std::vector<double> uphet_final;
        std::tie(uphet_final, result.mean_rms) = acc_cpp(
            uphet_comb,
            input.burst_abs_ref,
            input.burst_start,
            input.burst_end,
            input.outwidth,
            input.linesout);
        if constexpr (kEnableDetailedChromaPerf) {
            result.perf.acc_s +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t4).count();
        }
        if (input.keep_intermediates) result.uphet_final = uphet_final;
        if (!input.keep_intermediates) {
            std::vector<double>().swap(uphet_raw);
            std::vector<double>().swap(uphet_phase_comp);
            std::vector<double>().swap(uphet_after_chroma_deemph);
            std::vector<double>().swap(uphet_comb);
        }
        {
            const auto t5 = kEnableDetailedChromaPerf ? std::chrono::steady_clock::now()
                                                      : std::chrono::steady_clock::time_point{};
            result.chroma_u16 = chroma_to_u16_cpp(uphet_final);
            if constexpr (kEnableDetailedChromaPerf) {
                result.perf.to_u16_s +=
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t5).count();
            }
        }
    }
    return result;
}

}  // namespace vhsdecode_cpp

#include "vhsdecode_cpp/demod_block.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>

#include <fftw3.h>
#include <soxr.h>

namespace vhsdecode::cppport {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

void require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

void require_option_configs(const DemodBlockOptions& options) {
    if (options.enable_video_eq) {
        require(options.video_eq.has_value(),
                "demodblock C++ port: enable_video_eq requires video_eq config");
    }
    if (options.enable_chroma_trap) {
        require(options.chroma_trap.has_value(),
                "demodblock C++ port: enable_chroma_trap requires chroma_trap config");
    }
    if (options.enable_nldeemp) {
        require(options.nonlinear_deemphasis.has_value(),
                "demodblock C++ port: enable_nldeemp requires nonlinear_deemphasis config");
    }
    if (options.enable_subdeemp) {
        require(options.sub_deemphasis.has_value(),
                "demodblock C++ port: enable_subdeemp requires sub_deemphasis config");
    }
}

std::vector<std::complex<double>> fft_complex(const std::vector<std::complex<double>>& input) {
    const int n = static_cast<int>(input.size());
    std::vector<std::complex<double>> output(input.size());
    auto* in = reinterpret_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(n)));
    auto* out = reinterpret_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(n)));
    require(in && out, "fftw allocation failed");

    for (int i = 0; i < n; ++i) {
        in[i][0] = input[i].real();
        in[i][1] = input[i].imag();
    }

    fftw_plan plan = fftw_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    require(plan != nullptr, "fftw forward complex plan failed");
    fftw_execute(plan);

    for (int i = 0; i < n; ++i) {
        output[i] = {out[i][0], out[i][1]};
    }

    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);
    return output;
}

std::vector<std::complex<double>> ifft_complex(const std::vector<std::complex<double>>& input) {
    const int n = static_cast<int>(input.size());
    std::vector<std::complex<double>> output(input.size());
    auto* in = reinterpret_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(n)));
    auto* out = reinterpret_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(n)));
    require(in && out, "fftw allocation failed");

    for (int i = 0; i < n; ++i) {
        in[i][0] = input[i].real();
        in[i][1] = input[i].imag();
    }

    fftw_plan plan = fftw_plan_dft_1d(n, in, out, FFTW_BACKWARD, FFTW_ESTIMATE);
    require(plan != nullptr, "fftw inverse complex plan failed");
    fftw_execute(plan);

    const double inv_n = 1.0 / static_cast<double>(n);
    for (int i = 0; i < n; ++i) {
        output[i] = {out[i][0] * inv_n, out[i][1] * inv_n};
    }

    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);
    return output;
}

std::vector<std::complex<double>> fft_real(const std::vector<double>& input) {
    const int n = static_cast<int>(input.size());
    const int bins = (n / 2) + 1;
    std::vector<std::complex<double>> output(static_cast<std::size_t>(bins));

    auto* in = reinterpret_cast<double*>(fftw_malloc(sizeof(double) * static_cast<std::size_t>(n)));
    auto* out = reinterpret_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(bins)));
    require(in && out, "fftw allocation failed");

    std::copy(input.begin(), input.end(), in);

    fftw_plan plan = fftw_plan_dft_r2c_1d(n, in, out, FFTW_ESTIMATE);
    require(plan != nullptr, "fftw r2c plan failed");
    fftw_execute(plan);

    for (int i = 0; i < bins; ++i) {
        output[static_cast<std::size_t>(i)] = {out[i][0], out[i][1]};
    }

    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);
    return output;
}

std::vector<double> ifft_real(const std::vector<std::complex<double>>& input, std::size_t n) {
    const int bins = static_cast<int>(input.size());
    const int len = static_cast<int>(n);
    std::vector<double> output(n);

    auto* in = reinterpret_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(bins)));
    auto* out = reinterpret_cast<double*>(fftw_malloc(sizeof(double) * n));
    require(in && out, "fftw allocation failed");

    for (int i = 0; i < bins; ++i) {
        in[i][0] = input[static_cast<std::size_t>(i)].real();
        in[i][1] = input[static_cast<std::size_t>(i)].imag();
    }

    fftw_plan plan = fftw_plan_dft_c2r_1d(len, in, out, FFTW_ESTIMATE);
    require(plan != nullptr, "fftw c2r plan failed");
    fftw_execute(plan);

    const double inv_n = 1.0 / static_cast<double>(len);
    for (int i = 0; i < len; ++i) {
        output[static_cast<std::size_t>(i)] = out[i] * inv_n;
    }

    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);
    return output;
}

std::vector<double> real_part(const std::vector<std::complex<double>>& input) {
    std::vector<double> out(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        out[i] = input[i].real();
    }
    return out;
}

template <typename T>
void roll_in_place(std::vector<T>& values, int shift) {
    if (values.empty()) return;
    const int n = static_cast<int>(values.size());
    shift %= n;
    if (shift < 0) shift += n;
    std::rotate(values.rbegin(), values.rbegin() + shift, values.rend());
}

std::vector<double> abs_real(const std::vector<double>& input) {
    std::vector<double> out(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        out[i] = std::abs(input[i]);
    }
    return out;
}

double mean_of(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
}

std::vector<double> clip_vector(const std::vector<double>& values, double lo, double hi) {
    std::vector<double> out(values);
    for (double& v : out) {
        v = std::clamp(v, lo, hi);
    }
    return out;
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

std::vector<double> lfilter_zi_cpp(const IirFilter& filter) {
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
    IirFilter filt{};
    filt.b = {sec.b0, sec.b1, sec.b2};
    filt.a = {sec.a0, sec.a1, sec.a2};
    auto zi = lfilter_zi_cpp(filt);
    require(zi.size() == 2, "unexpected SOS zi length");
    return {zi[0], zi[1]};
}

std::pair<std::vector<double>, std::vector<double>> iir_lfilter_cpp(const IirFilter& filter,
                                                                    const std::vector<double>& input,
                                                                    std::vector<double> zi) {
    require(!filter.a.empty() && !filter.b.empty(), "iir filter coefficients are empty");
    const std::size_t n = std::max(filter.a.size(), filter.b.size());
    std::vector<double> a = filter.a;
    std::vector<double> b = filter.b;
    if (a.front() != 1.0) {
        const double a0 = a.front();
        for (double& v : a) v /= a0;
        for (double& v : b) v /= a0;
    }
    a.resize(n, 0.0);
    b.resize(n, 0.0);
    zi.resize(n - 1, 0.0);
    std::vector<double> out(input.size(), 0.0);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double x = input[i];
        double y = b[0] * x;
        if (!zi.empty()) y += zi[0];
        out[i] = y;
        for (std::size_t j = 0; j + 1 < zi.size(); ++j) {
            zi[j] = zi[j + 1] + b[j + 1] * x - a[j + 1] * y;
        }
        if (!zi.empty()) {
            zi.back() = b[n - 1] * x - a[n - 1] * y;
        }
    }
    return {std::move(out), std::move(zi)};
}

std::vector<double> sosfiltfilt_cpp(const SosFilter& filter, const std::vector<double>& input) {
    // DIVERGENCE: This now mirrors scipy.signal.sosfiltfilt(..., padtype="odd")
    // closely: default padlen, odd extension, sosfilt_zi-style steady-state
    // initialization, and fixed endpoint scaling per forward/backward pass.
    // The remaining non-literal difference is that the SOS filtering loop is a
    // local C++ implementation rather than SciPy/sci-rs internals.
    const std::size_t n_sections = filter.sections.size();
    const std::size_t zeros_b2 = std::count_if(filter.sections.begin(), filter.sections.end(),
                                               [](const SosSection& sec) { return sec.b2 == 0.0; });
    const std::size_t zeros_a2 = std::count_if(filter.sections.begin(), filter.sections.end(),
                                               [](const SosSection& sec) { return sec.a2 == 0.0; });
    std::size_t ntaps = (2 * n_sections) + 1;
    ntaps -= std::min(zeros_b2, zeros_a2);
    std::size_t edge = 3 * ntaps;
    if (input.size() <= 1) return input;
    edge = std::min(edge, input.size() - 1);
    const std::vector<double> ext = odd_ext(input, edge);
    std::vector<double> y = ext;
    double scale = 1.0;
    const double forward_x0 = y.front();
    for (const auto& sec : filter.sections) {
        const auto zi_base = sos_section_zi_cpp(sec);
        std::array<double, 2> zi = {zi_base[0] * forward_x0 * scale, zi_base[1] * forward_x0 * scale};
        double a0 = (sec.a0 == 0.0) ? 1.0 : sec.a0;
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
        double a0 = (sec.a0 == 0.0) ? 1.0 : sec.a0;
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

std::vector<double> iir_filtfilt_cpp(const IirFilter& filter, const std::vector<double>& input) {
    const std::size_t ntaps = std::max(filter.a.size(), filter.b.size());
    if (input.size() <= 1) return input;
    std::size_t edge = 3 * ntaps;
    edge = std::min(edge, input.size() - 1);
    const std::vector<double> ext = odd_ext(input, edge);
    auto zi_base = lfilter_zi_cpp(filter);
    std::vector<double> zi(zi_base.size());
    const double x0 = ext.front();
    for (std::size_t i = 0; i < zi.size(); ++i) zi[i] = zi_base[i] * x0;
    auto [y, zf1] = iir_lfilter_cpp(filter, ext, zi);
    std::reverse(y.begin(), y.end());
    const double y0 = y.front();
    for (std::size_t i = 0; i < zi.size(); ++i) zi[i] = zi_base[i] * y0;
    auto [y2, zf2] = iir_lfilter_cpp(filter, y, zi);
    (void)zf1;
    (void)zf2;
    std::reverse(y2.begin(), y2.end());
    if (edge == 0) return y2;
    return std::vector<double>(y2.begin() + static_cast<std::ptrdiff_t>(edge),
                               y2.end() - static_cast<std::ptrdiff_t>(edge));
}

std::vector<double> normalize_coeffs(const std::vector<double>& v, std::size_t n) {
    std::vector<double> out = v;
    out.resize(n, 0.0);
    return out;
}

std::vector<double> lfilter_with_state_cpp(const IirFilter& filter,
                                           const std::vector<double>& input,
                                           std::vector<double>& z) {
    std::size_t n = std::max(filter.a.size(), filter.b.size());
    if (n <= 1) return input;
    std::vector<double> a = normalize_coeffs(filter.a, n);
    std::vector<double> b = normalize_coeffs(filter.b, n);
    double a0 = a[0];
    require(std::abs(a0) > std::numeric_limits<double>::epsilon(), "iir a0 must be non-zero");
    for (double& x : a) x /= a0;
    for (double& x : b) x /= a0;

    std::size_t m = n - 1;
    if (z.size() != m) z.assign(m, 0.0);
    std::vector<double> out(input.size(), 0.0);

    for (std::size_t i = 0; i < input.size(); ++i) {
        double x = input[i];
        double y = b[0] * x + z[0];
        for (std::size_t j = 1; j < m; ++j) {
            z[j - 1] = z[j] + b[j] * x - a[j] * y;
        }
        z[m - 1] = b[m] * x - a[m] * y;
        out[i] = y;
    }
    return out;
}

std::vector<double> unwrap_hilbert_cpp(const std::vector<std::complex<double>>& hilbert,
                                       double freq_hz) {
    // DIVERGENCE: Python uses vhsd_rust.unwrap_hilbert(). The C++ port mirrors
    // the same effective phase-delta behavior by measuring the angle of the
    // conjugate product h[n] * conj(h[n-1]), wrapping negative deltas into
    // [0, 2*pi), then scaling to Hz. A previous absolute-angle subtraction
    // implementation matched easier fixtures but diverged badly on direct
    // 40 MHz input near the phase-wrap boundary.
    std::vector<double> out(hilbert.size(), 0.0);
    if (hilbert.empty()) return out;

    for (std::size_t i = 1; i < hilbert.size(); ++i) {
        double diff = 0.0;
        if (hilbert[i - 1].real() == 0.0 && hilbert[i - 1].imag() == 0.0) {
            diff = std::atan2(hilbert[i].imag(), hilbert[i].real());
        } else {
            const std::complex<double> prod = hilbert[i] * std::conj(hilbert[i - 1]);
            diff = std::atan2(prod.imag(), prod.real());
        }
        while (diff < 0.0) diff += kTau;
        while (diff >= kTau) diff -= kTau;
        out[i] = diff * (freq_hz / kTau);
    }
    return out;
}

std::vector<std::complex<double>> analytic_signal_cpp(const std::vector<double>& signal,
                                                      const std::vector<std::complex<double>>& hilbert_filter) {
    std::vector<std::complex<double>> raw_complex(signal.size());
    for (std::size_t i = 0; i < signal.size(); ++i) raw_complex[i] = {signal[i], 0.0};
    std::vector<std::complex<double>> spectrum = fft_complex(raw_complex);
    require(spectrum.size() == hilbert_filter.size(),
            "analytic_signal_cpp: hilbert filter size mismatch");
    for (std::size_t i = 0; i < spectrum.size(); ++i) {
        spectrum[i] *= hilbert_filter[i];
    }
    return ifft_complex(spectrum);
}

std::vector<std::complex<double>> diff_forward_complex(const std::vector<std::complex<double>>& input) {
    std::vector<std::complex<double>> out(input.size(), {0.0, 0.0});
    for (std::size_t i = 1; i < input.size(); ++i) {
        out[i] = input[i] - input[i - 1];
    }
    return out;
}

void replace_spikes_cpp(std::vector<double>& demod,
                        const std::vector<double>& demod_diffed,
                        double max_value,
                        int replace_start = 8,
                        int replace_end = 30) {
    require(demod.size() == demod_diffed.size(), "diff demod length mismatch");
    std::vector<std::size_t> to_fix;
    to_fix.reserve(demod.size());
    for (std::size_t i = 0; i < demod.size(); ++i) {
        if (demod[i] > max_value) to_fix.push_back(i);
    }
    for (std::size_t i : to_fix) {
        std::size_t start = (i < static_cast<std::size_t>(replace_start))
            ? 0 : i - static_cast<std::size_t>(replace_start);
        std::size_t end = std::min(i + static_cast<std::size_t>(replace_end), demod.size() - 1);

        double max_a = *std::max_element(demod.begin() + static_cast<std::ptrdiff_t>(start),
                                         demod.begin() + static_cast<std::ptrdiff_t>(end));
        double max_b = *std::max_element(demod_diffed.begin() + static_cast<std::ptrdiff_t>(start),
                                         demod_diffed.begin() + static_cast<std::ptrdiff_t>(end));
        if (max_b < max_a) {
            std::copy(demod_diffed.begin() + static_cast<std::ptrdiff_t>(start),
                      demod_diffed.begin() + static_cast<std::ptrdiff_t>(end),
                      demod.begin() + static_cast<std::ptrdiff_t>(start));
        }
    }
}

std::vector<double> shift_chroma_and_remove_dc(std::vector<double> out_chroma, int move) {
    roll_in_place(out_chroma, move);
    double avg = mean_of(out_chroma);
    for (double& v : out_chroma) v -= avg;
    return out_chroma;
}

struct ChromaFilterStages {
    std::vector<double> after_sos;
    std::vector<double> after_audio_notch;
    std::vector<double> after_video_notch;
    std::vector<double> final_out;
};

ChromaFilterStages demod_chroma_filt_cpp(const std::vector<double>& data,
                                         const SosFilter& filter,
                                         std::size_t blocklen,
                                         const std::optional<IirFilter>& notch,
                                         bool do_notch,
                                         int move,
                                         const std::optional<IirFilter>& audio_notch) {
    std::vector<double> slice(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(blocklen));
    ChromaFilterStages stages{};
    stages.after_sos = sosfiltfilt_cpp(filter, slice);
    std::vector<double> out_chroma = stages.after_sos;

    if (audio_notch.has_value()) {
        out_chroma = iir_filtfilt_cpp(*audio_notch, out_chroma);
    }
    stages.after_audio_notch = out_chroma;

    if (do_notch && notch.has_value()) {
        out_chroma = iir_filtfilt_cpp(*notch, out_chroma);
    }
    stages.after_video_notch = out_chroma;

    stages.final_out = shift_chroma_and_remove_dc(std::move(out_chroma), move);
    return stages;
}

std::vector<double> resample_soxr_cpp(const std::vector<double>& data,
                                      double input_rate,
                                      double output_rate) {
    if (data.empty()) return {};
    std::size_t out_len = static_cast<std::size_t>(
        std::llround(static_cast<double>(data.size()) * output_rate / input_rate));
    out_len = std::max<std::size_t>(1, out_len);
    std::vector<double> out(out_len);
    size_t idone = 0;
    size_t odone = 0;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);
    soxr_quality_spec_t q_spec = soxr_quality_spec(SOXR_QQ, 0);
    soxr_error_t err = soxr_oneshot(
        input_rate,
        output_rate,
        1,
        data.data(),
        data.size(),
        &idone,
        out.data(),
        out.size(),
        &odone,
        &io_spec,
        &q_spec,
        nullptr);
    require(err == nullptr, soxr_strerror(err));
    out.resize(odone);
    return out;
}

std::vector<double> pad_or_truncate_cpp(const std::vector<double>& data, std::size_t target_len) {
    if (data.size() == target_len) return data;
    if (data.size() > target_len) {
        return std::vector<double>(data.end() - static_cast<std::ptrdiff_t>(target_len), data.end());
    }
    std::vector<double> out = data;
    out.resize(target_len, data.empty() ? 0.0 : data.back());
    return out;
}

std::vector<double> chroma_trap_cpp(const std::vector<double>& luminance,
                                    const ChromaTrapConfig& cfg) {
    // DIVERGENCE: Python reaches libsoxr through the Python `soxr` package.
    // The C++ port calls the same native library directly through the official
    // C API, using the vendored dev-package header because the machine lacked
    // a system-wide `soxr.h`.
    double output_rate = cfg.fsc_mhz * 1e6 * static_cast<double>(cfg.multiplier);
    std::vector<double> downsampled = resample_soxr_cpp(luminance, cfg.fs_hz, output_rate);
    std::vector<double> delayed = downsampled;
    roll_in_place(delayed, cfg.delay);
    std::vector<double> combed(downsampled.size(), 0.0);
    for (std::size_t i = 0; i < combed.size(); ++i) {
        combed[i] = 0.5 * (downsampled[i] + delayed[i]);
    }
    std::vector<double> result = resample_soxr_cpp(combed, output_rate, cfg.fs_hz);
    return pad_or_truncate_cpp(result, luminance.size());
}

std::vector<double> apply_video_eq_cpp(const std::vector<double>& demod,
                                       const VideoEqConfig& cfg,
                                       DemodBlockState& state) {
    constexpr int overlap = 10;
    if (!state.video_eq_initialized) {
        state.video_eq_zi = lfilter_zi_cpp(cfg.filter);
        state.video_eq_initialized = true;
    }
    std::vector<double> ha = iir_filtfilt_cpp(cfg.filter, demod);
    std::vector<double> prefix(demod.begin(), demod.begin() + std::min<std::size_t>(demod.size(), overlap));
    std::vector<double> hb = lfilter_with_state_cpp(cfg.filter, prefix, state.video_eq_zi);

    std::vector<double> hc = ha;
    for (std::size_t i = 0; i < hb.size() && i < hc.size(); ++i) hc[i] = hb[i];
    std::vector<double> result(demod.size(), 0.0);
    for (std::size_t i = 0; i < demod.size(); ++i) {
        double hf = cfg.gain * hc[i];
        result[i] = demod[i] + (cfg.sharpness_level * hf);
    }
    return result;
}

std::vector<double> apply_nonlinear_deemphasis_cpp(const std::vector<double>& out_video,
                                                   const std::vector<std::complex<double>>& out_video_fft,
                                                   const DemodBlockFilters& filters,
                                                   const NonLinearDeemphasisConfig& cfg) {
    std::vector<std::complex<double>> hf_fft(out_video_fft.size());
    for (std::size_t i = 0; i < out_video_fft.size(); ++i) {
        hf_fft[i] = out_video_fft[i] * filters.nl_highpass_f[i];
    }
    std::vector<double> hf_part = ifft_real(hf_fft, out_video.size());
    hf_part = clip_vector(hf_part, cfg.highpass_limit_l, cfg.highpass_limit_h);
    std::vector<double> out = out_video;
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] -= hf_part[i];
    }
    return out;
}

std::vector<double> sub_deemphasis_cpp(const std::vector<double>& out_video,
                                       const std::vector<std::complex<double>>& out_video_fft,
                                       const DemodBlockFilters& filters,
                                       const SubDeemphasisConfig& cfg) {
    std::vector<std::complex<double>> hf_fft(out_video_fft.size());
    for (std::size_t i = 0; i < out_video_fft.size(); ++i) {
        hf_fft[i] = out_video_fft[i] * filters.nl_highpass_f[i];
    }
    std::vector<double> hf_part = ifft_real(hf_fft, out_video.size());

    double deviation = cfg.deviation / 2.0;
    auto analytic = analytic_signal_cpp(hf_part, filters.hilbert);
    std::vector<double> amplitude(analytic.size(), 0.0);
    for (std::size_t i = 0; i < analytic.size(); ++i) {
        amplitude[i] = std::abs(analytic[i]) / deviation;
    }
    amplitude = sosfiltfilt_cpp(filters.nl_amplitude_lpf, amplitude);
    amplitude = clip_vector(amplitude, 0.0, std::numeric_limits<double>::infinity());

    if (cfg.scaling_1.has_value()) {
        for (double& v : amplitude) v *= *cfg.scaling_1;
    }
    for (double& v : amplitude) v = std::pow(v, cfg.exponential_scaling);
    if (cfg.scaling_2.has_value()) {
        for (double& v : amplitude) v *= *cfg.scaling_2;
    }
    if (cfg.logistic_rate > 0.0) {
        for (double& v : amplitude) {
            v *= 1.0 / (1.0 + std::exp(-cfg.logistic_rate * (v - cfg.logistic_mid)));
        }
    }

    std::vector<double> out(out_video.size(), 0.0);
    for (std::size_t i = 0; i < out.size(); ++i) {
        double static_part = cfg.static_factor.has_value() ? hf_part[i] * *cfg.static_factor : 0.0;
        double scaled_hf = hf_part[i] * (1.0 - amplitude[i]);
        out[i] = out_video[i] - scaled_hf - static_part;
    }
    return out;
}

void validate_filter_sizes(const DemodBlockFilters& filters) {
    require(filters.blocklen > 0, "blocklen must be non-zero");
    require(filters.rf_video.size() == filters.blocklen, "RFVideo size must match blocklen");
    require(filters.hilbert.size() == filters.blocklen, "hilbert size must match blocklen");
    require(filters.fvideo.size() == (filters.blocklen / 2) + 1,
            "FVideo size must match rfft bin count");
    require(filters.fvideo05.size() == (filters.blocklen / 2) + 1,
            "FVideo05 size must match rfft bin count");
    if (!filters.fvideo_notch_f.empty()) {
        require(filters.fvideo_notch_f.size() == filters.blocklen,
                "FVideoNotchF size must match blocklen");
    }
    if (!filters.nl_highpass_f.empty()) {
        require(filters.nl_highpass_f.size() == (filters.blocklen / 2) + 1,
                "NLHighPassF size must match rfft bin count");
    }
}

}  // namespace

DemodBlockResult demodblock(const DemodBlockInput& input,
                            const DemodBlockFilters& filters,
                            const DemodBlockOptions& options,
                            double freq_hz,
                            DemodBlockState* state) {
    require_option_configs(options);
    validate_filter_sizes(filters);
    require(input.data.has_value() || input.fftdata.has_value(),
            "demodblock called without raw or FFT data");

    DemodBlockResult rv;

    std::vector<std::complex<double>> indata_fft;
    if (input.fftdata.has_value()) {
        indata_fft = *input.fftdata;
    } else {
        require(input.data->size() >= filters.blocklen, "raw input shorter than blocklen");
        std::vector<std::complex<double>> raw_complex(filters.blocklen);
        for (std::size_t i = 0; i < filters.blocklen; ++i) {
            raw_complex[i] = {(*input.data)[i], 0.0};
        }
        indata_fft = fft_complex(raw_complex);
    }

    if (input.data.has_value()) {
        require(input.data->size() >= filters.blocklen, "raw input shorter than blocklen");
        rv.data.assign(input.data->begin(),
                       input.data->begin() + static_cast<std::ptrdiff_t>(filters.blocklen));
    } else {
        rv.data = real_part(ifft_complex(indata_fft));
    }

    if (options.enable_video_notch && !filters.fvideo_notch_f.empty()) {
        for (std::size_t i = 0; i < indata_fft.size(); ++i) {
            indata_fft[i] *= filters.fvideo_notch_f[i];
        }
    }

    for (std::size_t i = 0; i < indata_fft.size(); ++i) {
        indata_fft[i] *= filters.rf_video[i];
    }

    {
        std::vector<std::complex<double>> raw_filtered_fft(indata_fft.size());
        for (std::size_t i = 0; i < indata_fft.size(); ++i) {
            raw_filtered_fft[i] = indata_fft[i] * filters.hilbert[i];
        }
        std::vector<double> raw_filtered = real_part(ifft_complex(raw_filtered_fft));
        raw_filtered = abs_real(raw_filtered);
        roll_in_place(raw_filtered, 4);
        rv.env = sosfiltfilt_cpp(filters.fenv_post, raw_filtered);
    }

    const double env_mean = mean_of(rv.env);
    const bool any_zero = std::any_of(rv.env.begin(), rv.env.end(), [](double v) { return v == 0.0; });

    if (!any_zero && options.enable_high_boost) {
        std::vector<double> data_filtered = real_part(ifft_complex(indata_fft));
        std::vector<double> high_part = sosfiltfilt_cpp(filters.rf_top, data_filtered);
        for (std::size_t i = 0; i < high_part.size(); ++i) {
            high_part[i] *= ((env_mean * 0.9) / rv.env[i]);
            high_part[i] *= options.high_boost;
        }

        std::vector<std::complex<double>> high_fft =
            fft_complex(std::vector<std::complex<double>>(high_part.begin(), high_part.end()));
        for (std::size_t i = 0; i < indata_fft.size(); ++i) {
            indata_fft[i] += high_fft[i];
        }
    }

    {
        std::vector<std::complex<double>> analytic_fft(indata_fft.size());
        for (std::size_t i = 0; i < indata_fft.size(); ++i) {
            analytic_fft[i] = indata_fft[i] * filters.hilbert[i];
        }
        rv.hilbert = ifft_complex(analytic_fft);
    }

    rv.demod_pre_spike = unwrap_hilbert_cpp(rv.hilbert, freq_hz);
    rv.demod = rv.demod_pre_spike;

    if (!options.disable_diff_demod && rv.demod.size() > 40) {
        auto max_it = std::max_element(rv.demod.begin() + 20, rv.demod.end() - 20);
        if (max_it != rv.demod.end() && *max_it > options.diff_demod_check_value) {
            std::vector<std::complex<double>> diffed_hilbert = diff_forward_complex(rv.hilbert);
            rv.demod_diffed = unwrap_hilbert_cpp(diffed_hilbert, freq_hz);
            replace_spikes_cpp(rv.demod, rv.demod_diffed, options.diff_demod_check_value);
        }
    }

    DemodBlockState local_state;
    DemodBlockState& use_state = state ? *state : local_state;

    if (options.enable_video_eq) {
        rv.demod = apply_video_eq_cpp(rv.demod, *options.video_eq, use_state);
    }

    if (options.enable_chroma_trap) {
        rv.demod = chroma_trap_cpp(rv.demod, *options.chroma_trap);
    }

    std::vector<std::complex<double>> demod_fft = fft_real(rv.demod);

    {
        std::vector<std::complex<double>> out_video_fft(demod_fft.size());
        for (std::size_t i = 0; i < demod_fft.size(); ++i) {
            out_video_fft[i] = demod_fft[i] * filters.fvideo[i];
        }
        rv.out_video = ifft_real(out_video_fft, filters.blocklen);

        if (options.enable_nldeemp) {
            rv.out_video = apply_nonlinear_deemphasis_cpp(rv.out_video, out_video_fft, filters,
                                                          *options.nonlinear_deemphasis);
        }

        if (options.enable_subdeemp) {
            rv.out_video = sub_deemphasis_cpp(rv.out_video, out_video_fft, filters,
                                              *options.sub_deemphasis);
        }
    }

    if (options.enable_fsc_notch && filters.fsc_notch.has_value()) {
        rv.out_video = iir_filtfilt_cpp(*filters.fsc_notch, rv.out_video);
    }

    {
        std::vector<std::complex<double>> out_video05_fft(demod_fft.size());
        for (std::size_t i = 0; i < demod_fft.size(); ++i) {
            out_video05_fft[i] = demod_fft[i] * filters.fvideo05[i];
        }
        rv.out_video05 = ifft_real(out_video05_fft, filters.blocklen);
        roll_in_place(rv.out_video05, -filters.f05_offset);
    }

    const std::vector<double>& chroma_source = options.color_under ? rv.data : rv.out_video;
    auto chroma_stages = demod_chroma_filt_cpp(chroma_source,
                                               filters.fvideo_burst,
                                               filters.blocklen,
                                               filters.fvideo_notch,
                                               options.enable_video_notch,
                                               options.chroma_offset,
                                               filters.chroma_audio_notch);
    rv.chroma_after_sos = chroma_stages.after_sos;
    rv.chroma_after_audio_notch = chroma_stages.after_audio_notch;
    rv.chroma_after_video_notch = chroma_stages.after_video_notch;
    rv.out_chroma = chroma_stages.final_out;

    return rv;
}

}  // namespace vhsdecode::cppport

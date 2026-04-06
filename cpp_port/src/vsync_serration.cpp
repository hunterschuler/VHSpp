#include "vhsdecode_cpp/vsync_serration.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>

namespace vhsdecode::cppport {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

void require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

double f_to_samples(double samp_rate, double frequency) {
    return samp_rate / frequency;
}

double t_to_samples(double samp_rate, double time) {
    return f_to_samples(samp_rate, 1.0 / time);
}

template <typename T>
std::vector<T> reversed_copy(const std::vector<T>& in) {
    return std::vector<T>(in.rbegin(), in.rend());
}

double median_inplace_cpp(std::vector<double>& values) {
    if (values.empty()) return 0.0;
    const std::size_t mid = values.size() / 2U;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    const double upper = values[mid];
    if ((values.size() & 1U) != 0U) return upper;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid - 1U), values.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (values[mid - 1U] + upper);
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
        for (int k = col; k < n; ++k) a[static_cast<std::size_t>(col) * n + k] /= diag;
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

std::pair<std::vector<double>, std::vector<double>> iir_lfilter_cpp(
    const std::vector<double>& b_in,
    const std::vector<double>& a_in,
    const std::vector<double>& input,
    std::vector<double> zi) {
    require(!a_in.empty() && !b_in.empty(), "iir filter coefficients are empty");
    const std::size_t n = std::max(a_in.size(), b_in.size());
    std::vector<double> a = a_in;
    std::vector<double> b = b_in;
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
        if (!zi.empty()) zi.back() = b[n - 1] * x - a[n - 1] * y;
    }
    return {std::move(out), std::move(zi)};
}

std::vector<double> iir_filtfilt_cpp(
    const std::vector<double>& b,
    const std::vector<double>& a,
    const std::vector<double>& zi_base,
    const std::vector<double>& input,
    std::size_t edge);

std::vector<double> iir_filtfilt_cpp(
    const std::vector<double>& b,
    const std::vector<double>& a,
    const std::vector<double>& input) {
    const auto zi_base = lfilter_zi_cpp(b, a);
    const std::size_t ntaps = std::max(a.size(), b.size());
    if (input.size() <= 1) return input;
    std::size_t edge = 3 * ntaps;
    edge = std::min(edge, input.size() - 1);
    return iir_filtfilt_cpp(b, a, zi_base, input, edge);
}

std::vector<double> iir_filtfilt_cpp(
    const std::vector<double>& b,
    const std::vector<double>& a,
    const std::vector<double>& zi_base,
    const std::vector<double>& input,
    std::size_t edge) {
    const std::size_t ntaps = std::max(a.size(), b.size());
    if (input.size() <= 1) return input;
    edge = std::min(edge, input.size() - 1);
    const std::vector<double> ext = odd_ext(input, edge);
    std::vector<double> zi(zi_base.size());
    const double x0 = ext.front();
    for (std::size_t i = 0; i < zi.size(); ++i) zi[i] = zi_base[i] * x0;
    auto [y, _zf1] = iir_lfilter_cpp(b, a, ext, zi);
    std::reverse(y.begin(), y.end());
    const double y0 = y.front();
    for (std::size_t i = 0; i < zi.size(); ++i) zi[i] = zi_base[i] * y0;
    auto [y2, _zf2] = iir_lfilter_cpp(b, a, y, zi);
    std::reverse(y2.begin(), y2.end());
    if (edge == 0) return y2;
    return std::vector<double>(y2.begin() + static_cast<std::ptrdiff_t>(edge),
                               y2.end() - static_cast<std::ptrdiff_t>(edge));
}

void iir_lfilter_into_cpp(const std::vector<double>& b,
                          const std::vector<double>& a,
                          const std::vector<double>& input,
                          const std::vector<double>& zi_init,
                          std::vector<double>& out,
                          std::vector<double>& zi) {
    const std::size_t n = a.size();
    zi.resize(n - 1, 0.0);
    std::copy(zi_init.begin(), zi_init.end(), zi.begin());
    out.resize(input.size());
    const double* const bptr = b.data();
    const double* const aptr = a.data();
    switch (n) {
    case 4:
        for (std::size_t i = 0; i < input.size(); ++i) {
            const double x = input[i];
            const double z0 = zi[0];
            const double z1 = zi[1];
            const double z2 = zi[2];
            const double y = (bptr[0] * x) + z0;
            out[i] = y;
            zi[0] = z1 + (bptr[1] * x) - (aptr[1] * y);
            zi[1] = z2 + (bptr[2] * x) - (aptr[2] * y);
            zi[2] = (bptr[3] * x) - (aptr[3] * y);
        }
        return;
    case 5:
        for (std::size_t i = 0; i < input.size(); ++i) {
            const double x = input[i];
            const double z0 = zi[0];
            const double z1 = zi[1];
            const double z2 = zi[2];
            const double z3 = zi[3];
            const double y = (bptr[0] * x) + z0;
            out[i] = y;
            zi[0] = z1 + (bptr[1] * x) - (aptr[1] * y);
            zi[1] = z2 + (bptr[2] * x) - (aptr[2] * y);
            zi[2] = z3 + (bptr[3] * x) - (aptr[3] * y);
            zi[3] = (bptr[4] * x) - (aptr[4] * y);
        }
        return;
    case 6:
        for (std::size_t i = 0; i < input.size(); ++i) {
            const double x = input[i];
            const double z0 = zi[0];
            const double z1 = zi[1];
            const double z2 = zi[2];
            const double z3 = zi[3];
            const double z4 = zi[4];
            const double y = (bptr[0] * x) + z0;
            out[i] = y;
            zi[0] = z1 + (bptr[1] * x) - (aptr[1] * y);
            zi[1] = z2 + (bptr[2] * x) - (aptr[2] * y);
            zi[2] = z3 + (bptr[3] * x) - (aptr[3] * y);
            zi[3] = z4 + (bptr[4] * x) - (aptr[4] * y);
            zi[4] = (bptr[5] * x) - (aptr[5] * y);
        }
        return;
    default:
        for (std::size_t i = 0; i < input.size(); ++i) {
            const double x = input[i];
            double y = bptr[0] * x;
            if (!zi.empty()) y += zi[0];
            out[i] = y;
            for (std::size_t j = 0; j + 1 < zi.size(); ++j) {
                zi[j] = zi[j + 1] + bptr[j + 1] * x - aptr[j + 1] * y;
            }
            if (!zi.empty()) zi.back() = bptr[n - 1] * x - aptr[n - 1] * y;
        }
        return;
    }
}

std::pair<int, double> buttord_cpp(double wp_hz,
                                   double ws_hz,
                                   double gpass_db,
                                   double gstop_db,
                                   double fs) {
    const double wp = 2.0 * fs * std::tan(kPi * wp_hz / fs);
    const double ws = 2.0 * fs * std::tan(kPi * ws_hz / fs);
    const double ep = std::sqrt(std::pow(10.0, gpass_db / 10.0) - 1.0);
    const double es = std::sqrt(std::pow(10.0, gstop_db / 10.0) - 1.0);
    const double n_real = std::log10(es / ep) / std::log10(ws / wp);
    const int order = std::max(1, static_cast<int>(std::ceil(n_real)));
    const double wn = wp / std::pow(ep, 1.0 / static_cast<double>(order));
    const double cutoff_hz = (fs / kPi) * std::atan(wn / (2.0 * fs));
    return {order, cutoff_hz};
}

struct DigitalZpk {
    std::vector<std::complex<double>> z;
    std::vector<std::complex<double>> p;
    std::complex<double> k{1.0, 0.0};
};

DigitalZpk buttap_zpk_cpp(int order) {
    DigitalZpk out;
    for (int m = 0; m < order; ++m) {
        const double theta = kPi * (2.0 * static_cast<double>(m) + 1.0 + static_cast<double>(order)) /
                             (2.0 * static_cast<double>(order));
        out.p.emplace_back(std::polar(1.0, theta));
    }
    return out;
}

DigitalZpk lp2lp_zpk_cpp(const DigitalZpk& in, double wo) {
    DigitalZpk out;
    for (const auto& z : in.z) out.z.push_back(z * wo);
    for (const auto& p : in.p) out.p.push_back(p * wo);
    const int degree = static_cast<int>(in.p.size()) - static_cast<int>(in.z.size());
    out.k = in.k * std::pow(wo, degree);
    return out;
}

DigitalZpk lp2hp_zpk_cpp(const DigitalZpk& in, double wo) {
    DigitalZpk out;
    for (const auto& z : in.z) out.z.push_back(wo / z);
    for (const auto& p : in.p) out.p.push_back(wo / p);
    const int degree = static_cast<int>(in.p.size()) - static_cast<int>(in.z.size());
    for (int i = 0; i < degree; ++i) out.z.emplace_back(0.0, 0.0);
    std::complex<double> num = in.k;
    for (const auto& z : in.z) num *= -z;
    std::complex<double> den{1.0, 0.0};
    for (const auto& p : in.p) den *= -p;
    out.k = num / den;
    return out;
}

DigitalZpk bilinear_zpk_cpp(const DigitalZpk& in, double fs) {
    DigitalZpk out;
    const std::complex<double> fs2{2.0 * fs, 0.0};
    for (const auto& z : in.z) out.z.push_back((fs2 + z) / (fs2 - z));
    for (const auto& p : in.p) out.p.push_back((fs2 + p) / (fs2 - p));
    const int degree = static_cast<int>(in.p.size()) - static_cast<int>(in.z.size());
    for (int i = 0; i < degree; ++i) out.z.emplace_back(-1.0, 0.0);
    std::complex<double> num = in.k;
    for (const auto& z : in.z) num *= (fs2 - z);
    std::complex<double> den{1.0, 0.0};
    for (const auto& p : in.p) den *= (fs2 - p);
    out.k = num / den;
    return out;
}

std::vector<std::complex<double>> poly_from_roots_cpp(const std::vector<std::complex<double>>& roots) {
    std::vector<std::complex<double>> poly{{1.0, 0.0}};
    for (const auto& root : roots) {
        std::vector<std::complex<double>> next(poly.size() + 1U, {0.0, 0.0});
        for (std::size_t i = 0; i < poly.size(); ++i) {
            next[i] += poly[i];
            next[i + 1U] -= poly[i] * root;
        }
        poly = std::move(next);
    }
    return poly;
}

std::pair<std::vector<double>, std::vector<double>> zpk_to_tf_cpp(const DigitalZpk& zpk) {
    auto bpoly = poly_from_roots_cpp(zpk.z);
    auto apoly = poly_from_roots_cpp(zpk.p);
    for (auto& c : bpoly) c *= zpk.k;
    std::vector<double> b(bpoly.size());
    std::vector<double> a(apoly.size());
    for (std::size_t i = 0; i < b.size(); ++i) b[i] = bpoly[i].real();
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = apoly[i].real();
    return {b, a};
}

std::pair<std::vector<double>, std::vector<double>> butter_digital_lowpass_ba_cpp(
    int order, double cutoff_hz, double fs) {
    auto zpk = bilinear_zpk_cpp(
        lp2lp_zpk_cpp(buttap_zpk_cpp(order), 2.0 * fs * std::tan(kPi * cutoff_hz / fs)), fs);
    return zpk_to_tf_cpp(zpk);
}

std::pair<std::vector<double>, std::vector<double>> butter_digital_highpass_ba_cpp(
    int order, double cutoff_hz, double fs) {
    auto zpk = bilinear_zpk_cpp(
        lp2hp_zpk_cpp(buttap_zpk_cpp(order), 2.0 * fs * std::tan(kPi * cutoff_hz / fs)), fs);
    return zpk_to_tf_cpp(zpk);
}

std::pair<std::vector<double>, std::vector<double>> firdes_lowpass_cpp(
    double samp_rate, double cutoff, double transition_width, int order_limit = 20) {
    const auto [order_raw, normal_cutoff] = buttord_cpp(cutoff, cutoff + transition_width, 3.0, 30.0, samp_rate);
    const int order = std::min(order_raw, order_limit);
    return butter_digital_lowpass_ba_cpp(order, normal_cutoff, samp_rate);
}

std::pair<std::vector<double>, std::vector<double>> firdes_highpass_cpp(
    double samp_rate, double cutoff, double transition_width, int order_limit = 20) {
    const auto [order_raw, normal_cutoff] = buttord_cpp(cutoff, cutoff + transition_width, 3.0, 30.0, samp_rate);
    const int order = std::min(order_raw, order_limit);
    return butter_digital_highpass_ba_cpp(order, normal_cutoff, samp_rate);
}

void normalize_filter_cpp(std::vector<double>& b, std::vector<double>& a) {
    require(!a.empty() && !b.empty(), "iir filter coefficients are empty");
    if (a.front() != 1.0) {
        const double a0 = a.front();
        for (double& v : a) v /= a0;
        for (double& v : b) v /= a0;
    }
    const std::size_t n = std::max(a.size(), b.size());
    a.resize(n, 0.0);
    b.resize(n, 0.0);
}

void zero_cross_det_into_cpp(const std::vector<double>& data, std::vector<int>& out) {
    out.clear();
    if (data.size() < 2) return;
    out.reserve(data.size() / 8);
    for (std::size_t i = 1; i < data.size(); ++i) {
        const double prev = data[i - 1];
        const double curr = data[i];
        const int sp = (prev > 0.0) - (prev < 0.0);
        const int sc = (curr > 0.0) - (curr < 0.0);
        if (sp != sc) out.push_back(static_cast<int>(i - 1));
    }
}

void argrelextrema_less_into_cpp(const std::vector<double>& data, std::vector<int>& out) {
    out.clear();
    if (data.size() < 3) return;
    for (std::size_t i = 1; i + 1 < data.size(); ++i) {
        if (data[i] < data[i - 1] && data[i] < data[i + 1]) {
            out.push_back(static_cast<int>(i));
        }
    }
}

void argrelextrema_less_shifted_into_cpp(const std::vector<double>& data,
                                         std::size_t begin,
                                         double bias,
                                         std::vector<int>& out) {
    out.clear();
    if (begin + 2 >= data.size()) return;
    for (std::size_t i = begin + 1; i + 1 < data.size(); ++i) {
        const double prev = data[i - 1] - bias;
        const double curr = data[i] - bias;
        const double next = data[i + 1] - bias;
        if (curr < prev && curr < next) {
            out.push_back(static_cast<int>(i - begin));
        }
    }
}


std::pair<double, double> get_serration_sync_levels_cpp(const std::vector<double>& serration) {
    const double half_amp = std::accumulate(serration.begin(), serration.end(), 0.0) /
                            static_cast<double>(serration.size());
    std::vector<double> peaks;
    std::vector<double> valleys;
    peaks.reserve(serration.size());
    valleys.reserve(serration.size());
    for (double v : serration) {
        if (v > half_amp) peaks.push_back(v);
        else valleys.push_back(v);
    }
    auto median_of = [](std::vector<double> values) {
        if (values.empty()) return 0.0;
        return median_inplace_cpp(values);
    };
    return {median_of(valleys), median_of(peaks)};
}

}  // namespace

VsyncSerration::StackableMA::StackableMA(int min_watermark, int window_average)
    : window_average_(window_average), min_watermark_(min_watermark) {}

void VsyncSerration::StackableMA::push(double value) { stack_.push_back(value); }

double VsyncSerration::StackableMA::mean() const {
    return std::accumulate(stack_.begin(), stack_.end(), 0.0) / static_cast<double>(stack_.size());
}

std::optional<double> VsyncSerration::StackableMA::pull() const {
    if (stack_.empty()) return std::nullopt;
    const std::size_t begin =
        stack_.size() >= static_cast<std::size_t>(window_average_)
            ? stack_.size() - static_cast<std::size_t>(window_average_)
            : 0U;
    const double sum = std::accumulate(stack_.begin() + static_cast<std::ptrdiff_t>(begin), stack_.end(), 0.0);
    return sum / static_cast<double>(stack_.size() - begin);
}

bool VsyncSerration::StackableMA::has_values() const {
    return static_cast<int>(stack_.size()) > min_watermark_;
}

int VsyncSerration::StackableMA::size() const { return static_cast<int>(stack_.size()); }

VsyncSerration::VsyncSerration(const VsyncSerrationConfig& config)
    : samp_rate_(config.sample_rate_hz / static_cast<double>(config.divisor)),
      divisor_(config.divisor),
      show_decoded_(config.show_decoded_serration),
      sync_levels_(1, 2),
      blank_levels_(1, 2) {
    const double fv = config.sysparams.fps * 2.0;
    const double fh = config.sysparams.fps * static_cast<double>(config.sysparams.frame_lines);

    const auto iir_vsync_env = firdes_lowpass_cpp(samp_rate_, fv * 5.0, 1e3);
    vsync_env_b_ = iir_vsync_env.first;
    vsync_env_a_ = iir_vsync_env.second;
    normalize_filter_cpp(vsync_env_b_, vsync_env_a_);
    vsync_env_zi_ = lfilter_zi_cpp(vsync_env_b_, vsync_env_a_);
    vsync_env_edge_ = 3 * vsync_env_a_.size();

    const auto iir_serration_base_lo = firdes_highpass_cpp(samp_rate_, fh, fh);
    serration_base_lo_b_ = iir_serration_base_lo.first;
    serration_base_lo_a_ = iir_serration_base_lo.second;
    normalize_filter_cpp(serration_base_lo_b_, serration_base_lo_a_);
    serration_base_lo_zi_ = lfilter_zi_cpp(serration_base_lo_b_, serration_base_lo_a_);
    serration_base_lo_edge_ = 3 * serration_base_lo_a_.size();
    const auto iir_serration_base_hi = firdes_lowpass_cpp(samp_rate_, fh, fh);
    serration_base_hi_b_ = iir_serration_base_hi.first;
    serration_base_hi_a_ = iir_serration_base_hi.second;
    normalize_filter_cpp(serration_base_hi_b_, serration_base_hi_a_);
    serration_base_hi_zi_ = lfilter_zi_cpp(serration_base_hi_b_, serration_base_hi_a_);
    serration_base_hi_edge_ = 3 * serration_base_hi_a_.size();

    const auto iir_serration_envelope_lo = firdes_lowpass_cpp(samp_rate_, fh / 3.0, fh / 2.0);
    serration_envelope_b_ = iir_serration_envelope_lo.first;
    serration_envelope_a_ = iir_serration_envelope_lo.second;
    normalize_filter_cpp(serration_envelope_b_, serration_envelope_a_);
    serration_envelope_zi_ = lfilter_zi_cpp(serration_envelope_b_, serration_envelope_a_);
    serration_envelope_edge_ = 3 * serration_envelope_a_.size();

    eq_pulselen_ = static_cast<int>(std::llround(
        t_to_samples(samp_rate_, config.sysparams.eq_pulse_us * 1e-6)));
    vsynclen_ = static_cast<int>(std::llround(f_to_samples(samp_rate_, fv)));
    linelen_ = static_cast<int>(std::llround(f_to_samples(samp_rate_, fh)));
    const double line_time = 1.0 / fh;
    const double vbi_time = 6.5 * line_time;
    vbi_time_range_ = {
        t_to_samples(samp_rate_, vbi_time * 3.0 / 4.0),
        t_to_samples(samp_rate_, vbi_time * 5.0 / 4.0)};
}

int VsyncSerration::get_eq_pulselen() const { return eq_pulselen_ * divisor_; }
int VsyncSerration::get_line_len() const { return linelen_ * divisor_; }

std::optional<std::pair<double, double>> VsyncSerration::pull_levels() const {
    const auto sync = sync_levels_.pull();
    const auto blank = blank_levels_.pull();
    if (!sync.has_value() || !blank.has_value()) return std::nullopt;
    return std::make_pair(*sync, *blank);
}

bool VsyncSerration::has_levels() const {
    return sync_levels_.has_values() && blank_levels_.has_values();
}

bool VsyncSerration::has_serration() const { return found_serration_; }

void VsyncSerration::push_levels(const std::pair<double, double>& levels) {
    sync_levels_.push(levels.first);
    blank_levels_.push(levels.second);
}

double VsyncSerration::mean_bias() const {
    return sync_level_bias_;
}

std::vector<double> VsyncSerration::remove_bias(const std::vector<double>& data) const {
    std::vector<double> out(data);
    for (double& v : out) v -= sync_level_bias_;
    return out;
}

std::vector<double> VsyncSerration::filtfilt_cached(const std::vector<double>& b,
                                                    const std::vector<double>& a,
                                                    const std::vector<double>& zi_base,
                                                    const std::vector<double>& input,
                                                    std::size_t edge) const {
    std::vector<double> output;
    filtfilt_into(b, a, zi_base, input, edge, output);
    return output;
}

void VsyncSerration::filtfilt_into(const std::vector<double>& b,
                                   const std::vector<double>& a,
                                   const std::vector<double>& zi_base,
                                   const std::vector<double>& input,
                                   std::size_t edge,
                                   std::vector<double>& output) const {
    if (input.size() <= 1) {
        output = input;
        return;
    }
    edge = std::min(edge, input.size() - 1);
    filt_ext_work_.resize(input.size() + 2U * edge);
    const double left = input.front();
    const double right = input.back();
    for (std::size_t i = 0; i < edge; ++i) {
        filt_ext_work_[i] = (2.0 * left) - input[edge - i];
    }
    std::copy(input.begin(), input.end(), filt_ext_work_.begin() + static_cast<std::ptrdiff_t>(edge));
    for (std::size_t i = 0; i < edge; ++i) {
        filt_ext_work_[edge + input.size() + i] = (2.0 * right) - input[input.size() - 2 - i];
    }

    filt_zi_work_.resize(zi_base.size());
    const double x0 = filt_ext_work_.front();
    for (std::size_t i = 0; i < zi_base.size(); ++i) filt_zi_work_[i] = zi_base[i] * x0;
    iir_lfilter_into_cpp(b, a, filt_ext_work_, filt_zi_work_, filt_stage_work_, filt_zi_work_);

    std::reverse(filt_stage_work_.begin(), filt_stage_work_.end());
    const double y0 = filt_stage_work_.front();
    for (std::size_t i = 0; i < zi_base.size(); ++i) filt_zi_work_[i] = zi_base[i] * y0;
    iir_lfilter_into_cpp(b, a, filt_stage_work_, filt_zi_work_, filt_stage2_work_, filt_zi_work_);
    std::reverse(filt_stage2_work_.begin(), filt_stage2_work_.end());

    if (edge == 0) {
        output = filt_stage2_work_;
        return;
    }
    const std::size_t out_size = filt_stage2_work_.size() - (2U * edge);
    output.resize(out_size);
    std::copy(filt_stage2_work_.begin() + static_cast<std::ptrdiff_t>(edge),
              filt_stage2_work_.begin() + static_cast<std::ptrdiff_t>(edge + out_size),
              output.begin());
}

double VsyncSerration::vsync_envelope_simple_into(const std::vector<double>& data,
                                                  std::vector<double>& output) const {
    hi_part_work_.resize(data.size());
    double min_value = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < data.size(); ++i) {
        const double v = data[i];
        if (v < min_value) min_value = v;
        hi_part_work_[i] = (v < 0.0) ? 0.0 : v;
    }
    filtfilt_into(vsync_env_b_, vsync_env_a_, vsync_env_zi_, hi_part_work_, vsync_env_edge_, output);
    return min_value;
}

double VsyncSerration::vsync_envelope_double_into(const std::vector<double>& data,
                                                  std::vector<double>& output) const {
    const int half = static_cast<int>(data.size() / 2U);
    const double min_value = vsync_envelope_simple_into(data, envelope_forward_work_);
    reverse_work_.resize(data.size());
    std::reverse_copy(data.begin(), data.end(), reverse_work_.begin());
    filtfilt_into(vsync_env_b_, vsync_env_a_, vsync_env_zi_, reverse_work_, vsync_env_edge_, envelope_reverse_result_work_);
    std::reverse(envelope_reverse_result_work_.begin(), envelope_reverse_result_work_.end());
    output = envelope_forward_work_;
    for (int i = 0; i < half; ++i) {
        output[static_cast<std::size_t>(i)] = envelope_reverse_result_work_[static_cast<std::size_t>(i)];
    }
    return min_value;
}

const std::vector<int>& VsyncSerration::power_ratio_search(const std::vector<double>& data) const {
    filtfilt_into(
        serration_base_lo_b_, serration_base_lo_a_, serration_base_lo_zi_, data,
        serration_base_lo_edge_, power_stage_work_);
    filtfilt_into(
        serration_base_hi_b_, serration_base_hi_a_, serration_base_hi_zi_, power_stage_work_,
        serration_base_hi_edge_, power_stage2_work_);
    for (double& v : power_stage2_work_) v *= v;
    filtfilt_into(
        serration_envelope_b_, serration_envelope_a_, serration_envelope_zi_, power_stage2_work_,
        serration_envelope_edge_, power_stage_work_);
    argrelextrema_less_into_cpp(power_stage_work_, serrations_work_);
    return serrations_work_;
}

std::optional<std::vector<int>> VsyncSerration::vsync_arbitrage(
    const std::vector<int>& where_allmin,
    const std::vector<int>& serrations,
    int datalen) const {
    std::vector<int> result;
    if (where_allmin.size() > 1U) {
        std::vector<int> valid_serrations;
        for (std::size_t id = 0; id < serrations.size(); ++id) {
            const int edge = serrations[id];
            for (int s_min : where_allmin) {
                const std::size_t next_serration_id = std::min(id + 1U, serrations.size() - 1U);
                if (edge <= s_min && s_min <= serrations[next_serration_id]) {
                    valid_serrations.push_back(edge);
                }
            }
        }
        for (int serration : valid_serrations) {
            if (serration - vsynclen_ >= 0 || serration + vsynclen_ <= datalen - 1) {
                result.push_back(serration);
            }
        }
    } else if (where_allmin.size() == 1U) {
        if (where_allmin[0] + vsynclen_ < datalen - 1) {
            result.push_back(where_allmin[0]);
            result.push_back(where_allmin[0] + vsynclen_);
        } else {
            result.push_back(where_allmin[0]);
            result.push_back(std::max(where_allmin[0] - vsynclen_, 0));
        }
    } else {
        return std::nullopt;
    }
    return result;
}

std::pair<bool, std::optional<std::pair<int, int>>> VsyncSerration::search_eq_pulses(
    const std::vector<double>& data,
    int pos,
    int linespan) {
    const int start = std::max(0, pos - linelen_ * linespan);
    const int end = std::min(static_cast<int>(data.size()) - 1, pos + linelen_ * linespan);
    if (start >= end) return {false, std::nullopt};
    std::vector<double> min_block(data.begin() + start, data.begin() + end);
    const double min_block_min = *std::min_element(min_block.begin(), min_block.end());
    std::vector<double> tmp = min_block;
    const double med = median_inplace_cpp(tmp);
    double level = (med - min_block_min) / 2.0;
    level += min_block_min;
    std::vector<double> zero_block(min_block.size());
    for (std::size_t i = 0; i < min_block.size(); ++i) zero_block[i] = min_block[i] - level;
    zero_cross_det_into_cpp(zero_block, zero_cross_work_);
    const std::vector<int>& sync_pulses = zero_cross_work_;
    if (sync_pulses.size() < 2U) return {false, std::nullopt};
    std::vector<int> where_min_diff;
    for (std::size_t i = 0; i + 1U < sync_pulses.size(); ++i) {
        const int diff_sync = sync_pulses[i + 1U] - sync_pulses[i];
        if (eq_pulselen_ * 0.2 < diff_sync && diff_sync < eq_pulselen_ * 5.0 / 4.0) {
            where_min_diff.push_back(static_cast<int>(i));
        }
    }
    if (where_min_diff.size() < 9U || where_min_diff.size() > 12U) {
        return {false, std::nullopt};
    }
    const int eq_s = sync_pulses[static_cast<std::size_t>(where_min_diff.front())];
    const int eq_e = std::min(
        static_cast<int>(sync_pulses[static_cast<std::size_t>(where_min_diff.back())] +
                         static_cast<int>(eq_pulselen_ / 2.0)),
        static_cast<int>(data.size()) - 1);
    const int data_s = eq_s + start;
    const int data_e = eq_e + start;
    if (data_s < 0 || data_e <= data_s || data_e > static_cast<int>(data.size())) {
        return {false, std::nullopt};
    }
    const std::vector<double> serration(data.begin() + data_s, data.begin() + data_e);
    if (!(vbi_time_range_.first < static_cast<double>(serration.size()) &&
          static_cast<double>(serration.size()) < vbi_time_range_.second)) {
        return {false, std::nullopt};
    }
    found_serration_ = true;
    push_levels(get_serration_sync_levels_cpp(serration));
    return {true, std::make_pair(data_s, data_e)};
}

std::optional<bool> VsyncSerration::vsync_envelope(const std::vector<double>& data, int padding) {
    const auto t_envelope_0 = std::chrono::steady_clock::now();
    const std::size_t prefix_len =
        static_cast<std::size_t>(std::min<int>(padding, static_cast<int>(data.size())));
    padded_work_.resize(prefix_len + data.size());
    for (std::size_t i = 0; i < prefix_len; ++i) {
        padded_work_[i] = data[prefix_len - 1U - i];
    }
    std::copy(data.begin(), data.end(), padded_work_.begin() + static_cast<std::ptrdiff_t>(prefix_len));

    sync_level_bias_ = vsync_envelope_double_into(padded_work_, envelope_result_work_);
    const std::size_t trim =
        static_cast<std::size_t>(std::min<int>(padding, static_cast<int>(envelope_result_work_.size())));
    argrelextrema_less_shifted_into_cpp(envelope_result_work_, trim, sync_level_bias_, minima_work_);
    const auto& where_allmin = minima_work_;
    if (show_decoded_) {
        last_debug_ = {};
        diff_work_.resize(envelope_result_work_.size() - trim);
        for (std::size_t i = 0; i < diff_work_.size(); ++i) {
            diff_work_[i] = envelope_result_work_[trim + i] - sync_level_bias_;
        }
        last_debug_.envelope = diff_work_;
        last_debug_.minima = where_allmin;
    }
    if (where_allmin.empty()) return std::nullopt;
    perf_stats_.envelope_s +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_envelope_0).count();
    const auto t_power_0 = std::chrono::steady_clock::now();
    const auto& serrations = power_ratio_search(padded_work_);
    perf_stats_.power_ratio_s +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_power_0).count();
    if (show_decoded_) {
        last_debug_.serrations = serrations;
    }
    auto where_min = vsync_arbitrage(where_allmin, serrations, static_cast<int>(padded_work_.size()));
    if (!where_min.has_value()) return std::nullopt;
    if (show_decoded_) {
        last_debug_.arbitrated = *where_min;
    }
    std::vector<int> serration_locs;
    int mask_len = linelen_ * 5;
    bool state = false;
    for (int w_min : *where_min) {
        const auto t_search_0 = std::chrono::steady_clock::now();
        auto [ok, locs] = search_eq_pulses(data, w_min);
        perf_stats_.search_eq_s +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_search_0).count();
        if (ok && locs.has_value()) {
            serration_locs.push_back(locs->first);
            mask_len = locs->second - locs->first;
            state = true;
        }
    }
    if (show_decoded_) {
        last_debug_.serration_locs = serration_locs;
        last_debug_.mask_len = mask_len;
        last_debug_.state = state;
    }
    return state;
}

void VsyncSerration::work(const std::vector<double>& data) {
    found_serration_ = false;
    const auto t_reduce_0 = std::chrono::steady_clock::now();
    if (divisor_ == 1) {
        perf_stats_.work_reduce_s +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_reduce_0).count();
        (void)vsync_envelope(data);
    } else {
        reduced_work_.clear();
        reduced_work_.reserve((data.size() + static_cast<std::size_t>(divisor_) - 1U) /
                              static_cast<std::size_t>(divisor_));
        for (std::size_t i = 0; i < data.size(); i += static_cast<std::size_t>(divisor_)) {
            reduced_work_.push_back(data[i]);
        }
        perf_stats_.work_reduce_s +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_reduce_0).count();
        (void)vsync_envelope(reduced_work_);
    }
    fieldcount_ += 1;
}

}  // namespace vhsdecode::cppport

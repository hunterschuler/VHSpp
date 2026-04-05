#include "vhsdecode_cpp/chroma_afc.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <stdexcept>

#include <fftw3.h>

namespace vhsdecode::cppport {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

void require(bool cond, const char* msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

struct DigitalZpk {
    std::vector<std::complex<double>> z;
    std::vector<std::complex<double>> p;
    std::complex<double> k{1.0, 0.0};
};

std::vector<double> normalize_coeffs(const std::vector<double>& v, std::size_t n) {
    std::vector<double> out = v;
    out.resize(n, 0.0);
    return out;
}

std::vector<double> odd_ext_cpp(const std::vector<double>& x, std::size_t edge) {
    if (edge == 0) return x;
    require(x.size() > edge, "odd_ext_cpp requires edge < len(x)");
    std::vector<double> ext;
    ext.reserve(x.size() + (2U * edge));
    const double left = x.front();
    const double right = x.back();
    for (std::size_t i = 0; i < edge; ++i) ext.push_back((2.0 * left) - x[edge - i]);
    ext.insert(ext.end(), x.begin(), x.end());
    for (std::size_t i = 0; i < edge; ++i) ext.push_back((2.0 * right) - x[x.size() - 2U - i]);
    return ext;
}

std::vector<double> lfilter_zi_cpp(const IirFilter& filter) {
    std::size_t n = std::max(filter.a.size(), filter.b.size());
    if (n <= 1) return {};
    std::vector<double> a = normalize_coeffs(filter.a, n);
    std::vector<double> b = normalize_coeffs(filter.b, n);
    const double a0 = a[0];
    require(std::abs(a0) > std::numeric_limits<double>::epsilon(), "iir a0 must be non-zero");
    for (double& x : a) x /= a0;
    for (double& x : b) x /= a0;
    const std::size_t m = n - 1;
    std::vector<std::vector<double>> mat(m, std::vector<double>(m, 0.0));
    std::vector<double> rhs(m, 0.0);
    for (std::size_t i = 0; i < m; ++i) {
        rhs[i] = b[i + 1] - a[i + 1] * b[0];
        mat[i][i] = 1.0 + a[i + 1];
        if (i > 0) mat[i][i - 1] = -1.0;
    }
    for (std::size_t col = 0; col < m; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < m; ++row) {
            if (std::abs(mat[row][col]) > std::abs(mat[pivot][col])) pivot = row;
        }
        if (pivot != col) {
            std::swap(mat[pivot], mat[col]);
            std::swap(rhs[pivot], rhs[col]);
        }
        const double div = mat[col][col];
        require(std::abs(div) > std::numeric_limits<double>::epsilon(), "singular lfilter_zi system");
        for (std::size_t j = col; j < m; ++j) mat[col][j] /= div;
        rhs[col] /= div;
        for (std::size_t row = 0; row < m; ++row) {
            if (row == col) continue;
            const double factor = mat[row][col];
            for (std::size_t j = col; j < m; ++j) mat[row][j] -= factor * mat[col][j];
            rhs[row] -= factor * rhs[col];
        }
    }
    return rhs;
}

std::pair<std::vector<double>, std::vector<double>> iir_lfilter_cpp(const IirFilter& filter,
                                                                    const std::vector<double>& input,
                                                                    std::vector<double> zi) {
    std::size_t n = std::max(filter.a.size(), filter.b.size());
    std::vector<double> a = normalize_coeffs(filter.a, n);
    std::vector<double> b = normalize_coeffs(filter.b, n);
    const double a0 = a[0];
    require(std::abs(a0) > std::numeric_limits<double>::epsilon(), "iir a0 must be non-zero");
    for (double& x : a) x /= a0;
    for (double& x : b) x /= a0;
    const std::size_t m = n - 1U;
    zi.resize(m, 0.0);
    std::vector<double> out(input.size(), 0.0);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double x = input[i];
        double y = b[0] * x;
        if (!zi.empty()) y += zi[0];
        out[i] = y;
        for (std::size_t j = 0; j + 1U < zi.size(); ++j) zi[j] = zi[j + 1U] + b[j + 1U] * x - a[j + 1U] * y;
        if (!zi.empty()) zi.back() = b[m] * x - a[m] * y;
    }
    return {std::move(out), std::move(zi)};
}

DigitalZpk buttap_zpk_cpp(int order) {
    DigitalZpk out;
    out.p.reserve(static_cast<std::size_t>(order));
    for (int m = -order + 1; m < order; m += 2) {
        const double angle = kPi * static_cast<double>(m) / (2.0 * static_cast<double>(order));
        out.p.emplace_back(-std::exp(std::complex<double>(0.0, angle)));
    }
    return out;
}

DigitalZpk lp2bp_zpk_cpp(const DigitalZpk& in, double wo, double bw) {
    DigitalZpk out;
    out.z.reserve(in.z.size() * 2U);
    out.p.reserve(in.p.size() * 2U);
    for (const auto& z : in.z) {
        const auto term = std::sqrt((bw * z) * (bw * z) - std::complex<double>(4.0 * wo * wo, 0.0));
        out.z.push_back(((bw * z) + term) / 2.0);
        out.z.push_back(((bw * z) - term) / 2.0);
    }
    for (const auto& p : in.p) {
        const auto term = std::sqrt((bw * p) * (bw * p) - std::complex<double>(4.0 * wo * wo, 0.0));
        out.p.push_back(((bw * p) + term) / 2.0);
        out.p.push_back(((bw * p) - term) / 2.0);
    }
    const int degree = static_cast<int>(in.p.size()) - static_cast<int>(in.z.size());
    for (int i = 0; i < degree; ++i) out.z.push_back(std::complex<double>(0.0, 0.0));
    out.k = in.k * std::pow(bw, degree);
    return out;
}

DigitalZpk bilinear_zpk_cpp(const DigitalZpk& in, double fs) {
    DigitalZpk out;
    const std::complex<double> fs2(2.0 * fs, 0.0);
    out.z.reserve(in.z.size());
    out.p.reserve(in.p.size());
    for (const auto& z : in.z) out.z.push_back((fs2 + z) / (fs2 - z));
    for (const auto& p : in.p) out.p.push_back((fs2 + p) / (fs2 - p));
    const int degree = static_cast<int>(in.p.size()) - static_cast<int>(in.z.size());
    for (int i = 0; i < degree; ++i) out.z.push_back(std::complex<double>(-1.0, 0.0));
    std::complex<double> num(1.0, 0.0);
    std::complex<double> den(1.0, 0.0);
    for (const auto& z : in.z) num *= (fs2 - z);
    for (const auto& p : in.p) den *= (fs2 - p);
    out.k = in.k * (num / den);
    return out;
}

DigitalZpk butter_digital_bandpass_zpk_cpp(int order, double low_hz, double high_hz, double fs) {
    const double warped_low = 2.0 * fs * std::tan(kPi * low_hz / fs);
    const double warped_high = 2.0 * fs * std::tan(kPi * high_hz / fs);
    const double bw = warped_high - warped_low;
    const double wo = std::sqrt(warped_low * warped_high);
    return bilinear_zpk_cpp(lp2bp_zpk_cpp(buttap_zpk_cpp(order), wo, bw), fs);
}

std::vector<std::complex<double>> pair_conjugates_cpp(std::vector<std::complex<double>> roots) {
    std::vector<std::complex<double>> ordered;
    ordered.reserve(roots.size());
    const double tol = 1e-9;
    while (!roots.empty()) {
        const auto r = roots.back();
        roots.pop_back();
        ordered.push_back(r);
        if (std::abs(r.imag()) < tol) continue;
        auto it = std::find_if(roots.begin(), roots.end(), [&](const std::complex<double>& x) {
            return std::abs(x.real() - r.real()) < tol && std::abs(x.imag() + r.imag()) < tol;
        });
        if (it != roots.end()) {
            ordered.push_back(*it);
            roots.erase(it);
        }
    }
    return ordered;
}

SosFilter zpk_to_sos_cpp(const DigitalZpk& zpk) {
    auto zeros = pair_conjugates_cpp(zpk.z);
    auto poles = pair_conjugates_cpp(zpk.p);
    require((zeros.size() % 2U) == 0U && (poles.size() % 2U) == 0U, "zpk_to_sos_cpp expects even root count");
    const std::size_t sections = std::max(zeros.size(), poles.size()) / 2U;
    zeros.resize(sections * 2U, std::complex<double>(0.0, 0.0));
    poles.resize(sections * 2U, std::complex<double>(0.0, 0.0));
    SosFilter sos;
    sos.sections.reserve(sections);
    const auto all_pm_one = std::all_of(zeros.begin(), zeros.end(), [](const std::complex<double>& z) {
        return std::abs(std::abs(z.real()) - 1.0) < 1e-9 && std::abs(z.imag()) < 1e-9;
    });
    if (all_pm_one) {
        std::vector<std::pair<std::complex<double>, std::complex<double>>> pole_pairs;
        pole_pairs.reserve(sections);
        for (std::size_t i = 0; i < sections; ++i) {
            pole_pairs.push_back({poles[2U * i], poles[(2U * i) + 1U]});
        }
        std::sort(pole_pairs.begin(), pole_pairs.end(), [](const auto& lhs, const auto& rhs) {
            return std::abs(lhs.first * lhs.second) < std::abs(rhs.first * rhs.second);
        });
        const std::size_t neg_sections = static_cast<std::size_t>(
            std::count_if(zeros.begin(), zeros.end(), [](const std::complex<double>& z) { return z.real() < 0.0; }) / 2);
        for (std::size_t i = 0; i < sections; ++i) {
            const auto [p1, p2] = pole_pairs[i];
            const double zero_sign = (i < neg_sections) ? -1.0 : 1.0;
            sos.sections.push_back({
                1.0,
                -2.0 * zero_sign,
                1.0,
                1.0,
                (-(p1 + p2)).real(),
                (p1 * p2).real(),
            });
        }
    } else {
        for (std::size_t i = 0; i < sections; ++i) {
            const auto z1 = zeros[2U * i];
            const auto z2 = zeros[(2U * i) + 1U];
            const auto p1 = poles[2U * i];
            const auto p2 = poles[(2U * i) + 1U];
            sos.sections.push_back({
                1.0,
                (-(z1 + z2)).real(),
                (z1 * z2).real(),
                1.0,
                (-(p1 + p2)).real(),
                (p1 * p2).real(),
            });
        }
    }
    if (!sos.sections.empty()) {
        sos.sections[0].b0 *= zpk.k.real();
        sos.sections[0].b1 *= zpk.k.real();
        sos.sections[0].b2 *= zpk.k.real();
    }
    return sos;
}

std::vector<float> gen_wave_at_frequency_cpp(double frequency_hz,
                                             double sample_frequency_hz,
                                             int num_samples,
                                             bool cosine = false) {
    std::vector<float> out(static_cast<std::size_t>(num_samples));
    const double wave_scale = frequency_hz / sample_frequency_hz;
    for (int i = 0; i < num_samples; ++i) {
        double phase = kTau * wave_scale * static_cast<double>(i);
        out[static_cast<std::size_t>(i)] = static_cast<float>(cosine ? std::cos(phase) : std::sin(phase));
    }
    return out;
}

std::vector<std::complex<double>> fft_complex_cpp(const std::vector<double>& input) {
    const int n = static_cast<int>(input.size());
    auto* in = reinterpret_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(n)));
    auto* out = reinterpret_cast<fftw_complex*>(
        fftw_malloc(sizeof(fftw_complex) * static_cast<std::size_t>(n)));
    require(in && out, "fftw allocation failed");

    for (int i = 0; i < n; ++i) {
        in[i][0] = input[static_cast<std::size_t>(i)];
        in[i][1] = 0.0;
    }

    fftw_plan plan = fftw_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
    require(plan != nullptr, "fftw complex fft plan failed");
    fftw_execute(plan);

    std::vector<std::complex<double>> result(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        result[static_cast<std::size_t>(i)] = {out[i][0], out[i][1]};
    }

    fftw_destroy_plan(plan);
    fftw_free(in);
    fftw_free(out);
    return result;
}

std::vector<double> iir_filtfilt_cpp(const IirFilter& filter, const std::vector<double>& input) {
    if (input.size() <= 1) return input;
    const std::size_t ntaps = std::max(filter.a.size(), filter.b.size());
    const std::size_t edge = std::min<std::size_t>(3U * ntaps, input.size() - 1U);
    const auto ext = odd_ext_cpp(input, edge);
    auto zi_base = lfilter_zi_cpp(filter);
    std::vector<double> zi = zi_base;
    for (std::size_t i = 0; i < zi.size(); ++i) zi[i] *= ext.front();
    auto [y, _zf0] = iir_lfilter_cpp(filter, ext, zi);
    std::reverse(y.begin(), y.end());
    zi = zi_base;
    for (std::size_t i = 0; i < zi.size(); ++i) zi[i] *= y.front();
    auto [y2, _zf1] = iir_lfilter_cpp(filter, y, zi);
    std::reverse(y2.begin(), y2.end());
    return std::vector<double>(y2.begin() + static_cast<std::ptrdiff_t>(edge),
                               y2.end() - static_cast<std::ptrdiff_t>(edge));
}

IirFilter lowpass_first_order_cpp(double fs, double cutoff_hz) {
    const double k = std::tan(kPi * cutoff_hz / fs);
    const double norm = 1.0 / (1.0 + k);
    return {{k * norm, k * norm}, {1.0, (k - 1.0) * norm}};
}

IirFilter highpass_first_order_cpp(double fs, double cutoff_hz) {
    const double k = std::tan(kPi * cutoff_hz / fs);
    const double norm = 1.0 / (1.0 + k);
    return {{norm, -norm}, {1.0, (k - 1.0) * norm}};
}

IirFilter biquad_to_ba(double b0, double b1, double b2, double a0, double a1, double a2) {
    return {{b0 / a0, b1 / a0, b2 / a0}, {1.0, a1 / a0, a2 / a0}};
}

IirFilter lowpass_biquad_cpp(double fs, double cutoff_hz, double q) {
    const double w0 = kTau * cutoff_hz / fs;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    return biquad_to_ba(
        (1.0 - cosw0) / 2.0,
        1.0 - cosw0,
        (1.0 - cosw0) / 2.0,
        1.0 + alpha,
        -2.0 * cosw0,
        1.0 - alpha);
}

IirFilter highpass_biquad_cpp(double fs, double cutoff_hz, double q) {
    const double w0 = kTau * cutoff_hz / fs;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    return biquad_to_ba(
        (1.0 + cosw0) / 2.0,
        -(1.0 + cosw0),
        (1.0 + cosw0) / 2.0,
        1.0 + alpha,
        -2.0 * cosw0,
        1.0 - alpha);
}

std::vector<double> butterworth_qs(int order) {
    std::vector<double> qs;
    for (int k = 1; k <= order / 2; ++k) {
        qs.push_back(1.0 / (2.0 * std::cos(kPi * (2.0 * static_cast<double>(k) - 1.0) /
                                           (2.0 * static_cast<double>(order)))));
    }
    return qs;
}

std::vector<IirFilter> make_lowpass_chain_cpp(double fs, double cutoff_hz, int order) {
    std::vector<IirFilter> out;
    if (order % 2) out.push_back(lowpass_first_order_cpp(fs, cutoff_hz));
    for (double q : butterworth_qs(order)) out.push_back(lowpass_biquad_cpp(fs, cutoff_hz, q));
    return out;
}

std::vector<IirFilter> make_highpass_chain_cpp(double fs, double cutoff_hz, int order) {
    std::vector<IirFilter> out;
    if (order % 2) out.push_back(highpass_first_order_cpp(fs, cutoff_hz));
    for (double q : butterworth_qs(order)) out.push_back(highpass_biquad_cpp(fs, cutoff_hz, q));
    return out;
}

SosSection bandpass_biquad_sos_cpp(double fs, double low_hz, double high_hz, double q_scale) {
    const double center_hz = std::sqrt(low_hz * high_hz);
    const double bandwidth_hz = high_hz - low_hz;
    const double q = (center_hz / bandwidth_hz) * q_scale;
    const double w0 = kTau * center_hz / fs;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    return {alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * cosw0, 1.0 - alpha};
}

SosFilter make_bandpass_sos_cpp(double fs, double low_hz, double high_hz, int order) {
    // DIVERGENCE: This mirrors scipy.signal.butter(..., output=\"sos\") via a
    // digital ZPK path plus local root pairing, rather than the earlier RBJ
    // biquad approximation. The remaining non-literal part is the local
    // zpk_to_sos_cpp() root-pairing / gain distribution policy.
    return zpk_to_sos_cpp(butter_digital_bandpass_zpk_cpp(order, low_hz, high_hz, fs));
}

}  // namespace

ChromaAfc::StackableMA::StackableMA(int min_watermark, int window_average)
    : window_average_(window_average), min_watermark_(min_watermark) {}

void ChromaAfc::StackableMA::push(double value) { stack_.push_back(value); }

double ChromaAfc::StackableMA::mean() const {
    return std::accumulate(stack_.begin(), stack_.end(), 0.0) / static_cast<double>(stack_.size());
}

std::optional<double> ChromaAfc::StackableMA::pull() {
    if (stack_.empty()) return std::nullopt;
    if (static_cast<int>(stack_.size()) >= window_average_) {
        stack_.erase(stack_.begin(), stack_.end() - window_average_);
    }
    return mean();
}

bool ChromaAfc::StackableMA::has_values() const { return static_cast<int>(stack_.size()) > min_watermark_; }
std::optional<double> ChromaAfc::StackableMA::current() const {
    if (stack_.empty()) return std::nullopt;
    return stack_.back();
}
int ChromaAfc::StackableMA::size() const { return static_cast<int>(stack_.size()); }
double ChromaAfc::StackableMA::work(double value) {
    push(value);
    return pull().value_or(value);
}

ChromaAfc::ChromaAfc(const ChromaAfcConfig& config) : config_(config) {
    fv_ = config.fps * 2.0;
    fh_ = config.fps * static_cast<double>(config.frame_lines);
    out_sample_rate_mhz_ = config.fsc_mhz * 4.0;
    samp_rate_hz_ = out_sample_rate_mhz_ * 1e6;
    out_frequency_half_mhz_ = out_sample_rate_mhz_ / 2.0;
    fieldlen_ = config.outlinelen * config.max_field_lines;

    const double percent = 100.0 * (-1.0 + (config.color_under_carrier_hz + (2.0 * fh_)) /
                                              config.color_under_carrier_hz);
    max_f_dev_percent_down_ = percent;
    max_f_dev_percent_up_ = percent;

    fsc_wave_ = gen_wave_at_frequency_cpp(config.fsc_mhz * 1e6, samp_rate_hz_, fieldlen_);
    fsc_cos_wave_ = gen_wave_at_frequency_cpp(config.fsc_mhz * 1e6, samp_rate_hz_, fieldlen_, true);

    if (config.do_cafc) {
        meas_stack_.emplace(0, 8192);
        chroma_log_drift_.emplace(0, 8192);
        chroma_bias_drift_.emplace(0, 6);

        auto [min_f, max_f] = getBandTolerance();
        const double trans_lo = config.color_under_carrier_hz * transition_expand_ * (max_f - 1.0);
        const double trans_hi = config.color_under_carrier_hz * transition_expand_ * (1.0 - min_f);
        auto lo = make_highpass_chain_cpp(samp_rate_hz_, config.color_under_carrier_hz, 11);
        auto hi = make_lowpass_chain_cpp(samp_rate_hz_, config.color_under_carrier_hz, 11);
        (void)trans_lo;
        (void)trans_hi;
        narrowband_filters_.insert(narrowband_filters_.end(), lo.begin(), lo.end());
        narrowband_filters_.insert(narrowband_filters_.end(), hi.begin(), hi.end());

        on_linearization_ = config.linearize;
        if (config.linearize) {
            fit();
            on_linearization_ = false;
        }
    }

    setCC(config.color_under_carrier_hz);
}

double ChromaAfc::getSampleRate() const { return samp_rate_hz_; }
double ChromaAfc::getOutFreqHalf() const { return out_frequency_half_mhz_ * 1e6; }

void ChromaAfc::setCC(double fcc_hz) {
    cc_freq_mhz_ = fcc_hz / 1e6;
    genHetC();
}

double ChromaAfc::getCC() const { return cc_freq_mhz_ * 1e6; }
double ChromaAfc::getCCPhase() const { return cc_phase_; }
void ChromaAfc::setCCPhase(double phase_rad) {
    cc_phase_ = phase_rad;
    genHetC();
}
void ChromaAfc::resetCCPhase() { cc_phase_ = 0.0; }
void ChromaAfc::resetCC() { setCC(config_.color_under_carrier_hz); }

const std::vector<std::vector<float>>& ChromaAfc::getChromaHet() const { return chroma_heterodyne_; }
std::pair<const std::vector<float>&, const std::vector<float>&> ChromaAfc::getFSCWaves() const {
    return {fsc_wave_, fsc_cos_wave_};
}

void ChromaAfc::genHetC() { chroma_heterodyne_ = genHetCDirect(); }

std::vector<std::vector<float>> ChromaAfc::genHetCDirect() const {
    const double het_freq_hz = (config_.fsc_mhz + cc_freq_mhz_) * 1e6;
    const double het_wave_scale = het_freq_hz / samp_rate_hz_;
    std::vector<std::vector<float>> out(4, std::vector<float>(static_cast<std::size_t>(fieldlen_)));
    for (int i = 0; i < fieldlen_; ++i) {
        const double phase = kTau * het_wave_scale * static_cast<double>(i) + cc_phase_;
        out[0][static_cast<std::size_t>(i)] = static_cast<float>(-std::cos(phase));
        out[1][static_cast<std::size_t>(i)] = static_cast<float>(-std::cos(phase + kPi / 2.0));
        out[2][static_cast<std::size_t>(i)] = static_cast<float>(-std::cos(phase + kPi));
        out[3][static_cast<std::size_t>(i)] = static_cast<float>(-std::cos(phase + 3.0 * kPi / 2.0));
    }
    return out;
}

std::pair<double, double> ChromaAfc::getBandTolerance() const {
    return {(100.0 - max_f_dev_percent_down_) / 100.0, (100.0 + max_f_dev_percent_up_) / 100.0};
}

double ChromaAfc::compensate(double x) const {
    return x * corrector_[0] + corrector_[1];
}

double ChromaAfc::fineTune(double freq, double max_step) const {
    double tune = freq;
    while (std::abs(tune - config_.color_under_carrier_hz) >= max_step) {
        tune -= (tune > config_.color_under_carrier_hz) ? max_step : -max_step;
    }
    double plus = tune + max_step;
    double minus = tune - max_step;
    auto dist = [&](double v) { return std::abs(v - config_.color_under_carrier_hz); };
    if (dist(tune) < dist(plus) && dist(tune) < dist(minus)) return tune;
    return dist(plus) < dist(minus) ? plus : minus;
}

double ChromaAfc::fftCenterFreq(const std::vector<double>& data) {
    auto sig_fft = fft_complex_cpp(data);
    const double time_step = 1.0 / samp_rate_hz_;
    std::vector<double> sample_freq_full(sig_fft.size(), 0.0);
    for (std::size_t i = 0; i < sig_fft.size(); ++i) {
        double f = static_cast<double>(i) / (static_cast<double>(sig_fft.size()) * time_step);
        if (i > sig_fft.size() / 2U) f -= 1.0 / time_step;
        sample_freq_full[i] = f;
    }

    std::vector<double> freqs;
    std::vector<double> power;
    std::vector<double> phase;
    freqs.reserve(sig_fft.size() / 2U);
    power.reserve(sig_fft.size() / 2U);
    phase.reserve(sig_fft.size() / 2U);

    for (std::size_t i = 0; i < sig_fft.size(); ++i) {
        if (sample_freq_full[i] > 0.0) {
            freqs.push_back(sample_freq_full[i]);
            power.push_back(std::norm(sig_fft[i]));
            phase.push_back(std::arg(sig_fft[i]));
        }
    }
    require(!freqs.empty(), "fftCenterFreq got empty positive spectrum");

    if (on_linearization_) {
        auto it = std::max_element(power.begin(), power.end());
        std::size_t idx = static_cast<std::size_t>(std::distance(power.begin(), it));
        cc_phase_ = phase[idx];
        return freqs[idx];
    }

    const double max_power = *std::max_element(power.begin(), power.end());
    double best_peak = 0.0;
    double best_dist = std::numeric_limits<double>::infinity();
    for (std::size_t i = 1; i + 1 < power.size(); ++i) {
        if (power[i] >= power[i - 1] && power[i] >= power[i + 1] && power[i] >= max_power * power_threshold_) {
            double dist = std::abs(freqs[i] - config_.color_under_carrier_hz);
            if (dist < best_dist) {
                best_dist = dist;
                best_peak = freqs[i];
            }
        }
    }
    if (best_peak == 0.0) {
        auto it = std::max_element(power.begin(), power.end());
        std::size_t idx = static_cast<std::size_t>(std::distance(power.begin(), it));
        best_peak = freqs[idx];
    }

    const double fine_tune_threshold =
        (std::string(config_.tape_format) == "UMATIC") ? fh_ :
        (std::string(config_.tape_format) == "BETAMAX") ? (fh_ / 2.0) :
                                                          (fh_ / 4.0);
    const double carrier_freq = fineTune(best_peak, fine_tune_threshold);

    // DIVERGENCE PRESERVED ON PURPOSE:
    // Upstream Python does an exact equality search against the full fftfreq()
    // output after fineTune(). If the tuned frequency does not land exactly on a
    // bin, it falls back to phase 0 instead of taking the nearest bin phase.
    // That behavior looks ugly, but it is the literal parity target here.
    cc_phase_ = 0.0;
    for (std::size_t i = 0; i < sample_freq_full.size(); ++i) {
        if (sample_freq_full[i] == carrier_freq) {
            cc_phase_ = std::arg(sig_fft[i]);
            break;
        }
    }
    return carrier_freq;
}

double ChromaAfc::measureCenterFreq(const std::vector<double>& data) {
    std::vector<double> filtered = data;
    for (const auto& filt : narrowband_filters_) filtered = iir_filtfilt_cpp(filt, filtered);
    return fftCenterFreq(filtered);
}

std::vector<std::vector<double>> ChromaAfc::tableset(int sample_size, int points) {
    std::vector<std::vector<double>> means;
    means.reserve(static_cast<std::size_t>(points));
    auto [min_f, max_f] = getBandTolerance();
    for (int ix = 0; ix < points; ++ix) {
        const double t = static_cast<double>(ix) / static_cast<double>(points - 1);
        const double freq = config_.color_under_carrier_hz * min_f +
                            t * (config_.color_under_carrier_hz * max_f - config_.color_under_carrier_hz * min_f);
        std::vector<float> wave = gen_wave_at_frequency_cpp(freq, samp_rate_hz_, sample_size);
        std::vector<double> fdc_wave(wave.begin(), wave.end());
        setCC(freq);
        double mean = measureCenterFreq(fdc_wave);
        means.push_back({freq, mean});
    }
    return means;
}

void ChromaAfc::fit() {
    auto table = tableset(fieldlen_);
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (const auto& row : table) {
        sx += row[0];
        sy += row[1];
        sxx += row[0] * row[0];
        sxy += row[0] * row[1];
    }
    const double n = static_cast<double>(table.size());
    const double denom = (n * sxx) - (sx * sx);
    require(std::abs(denom) > 0.0, "ChromaAfc::fit degenerate regression");
    const double m = ((n * sxy) - (sx * sy)) / denom;
    const double c = (sy - (m * sx)) / n;
    corrector_[0] = m;
    corrector_[1] = c;
}

SosFilter ChromaAfc::getChromaBandpass() const {
    return make_bandpass_sos_cpp(
        config_.demod_rate_hz,
        config_.chroma_bpf_lower_hz,
        getCC() * config_.under_ratio,
        config_.chroma_bandpass_order);
}

SosFilter ChromaAfc::getChromaBandpassFinal(bool color_under_format) const {
    const double lower = color_under_format ? ((config_.color_under_carrier_hz / 1e6) * 0.9) : 0.1;
    const double upper = color_under_format ? ((config_.color_under_carrier_hz / 1e6) * 0.75) : 0.1;
    return make_bandpass_sos_cpp(
        out_sample_rate_mhz_ * 1e6,
        (config_.fsc_mhz - lower) * 1e6,
        (config_.fsc_mhz + upper) * 1e6,
        4);
}

ChromaAfcMeasurement ChromaAfc::freqOffset(const std::vector<double>& chroma, bool adjustf) {
    auto [min_f, max_f] = getBandTolerance();
    const double comp_f = compensate(measureCenterFreq(chroma));
    const double clipped = std::clamp(comp_f,
                                      config_.color_under_carrier_hz * min_f,
                                      config_.color_under_carrier_hz * max_f);

    double freq_cc = getCC();
    if (adjustf) {
        freq_cc = meas_stack_ ? meas_stack_->work(clipped) : clipped;
    }
    setCC(freq_cc);

    ChromaAfcMeasurement out;
    out.spec_hz = config_.color_under_carrier_hz;
    out.measured_hz = freq_cc;
    out.long_term_offset_hz = chroma_log_drift_ ? chroma_log_drift_->work(freq_cc - config_.color_under_carrier_hz)
                                                : (freq_cc - config_.color_under_carrier_hz);
    out.cc_phase_rad = cc_phase_;
    return out;
}

}  // namespace vhsdecode::cppport

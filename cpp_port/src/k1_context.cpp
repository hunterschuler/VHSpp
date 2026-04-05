#include "vhsdecode_cpp/k1_context.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <sstream>
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

double sqr(double v) { return v * v; }

struct DigitalZpk {
    std::vector<std::complex<double>> z;
    std::vector<std::complex<double>> p;
    std::complex<double> k{1.0, 0.0};
};

std::vector<std::complex<double>> build_hilbert_cpp(std::size_t fft_size) {
    require((fft_size % 2U) == 0U, "build_hilbert_cpp requires even fft size");

    std::vector<std::complex<double>> out(fft_size, {0.0, 0.0});
    out[0] = {1.0, 0.0};
    out[fft_size / 2] = {1.0, 0.0};
    for (std::size_t i = 1; i < (fft_size / 2); ++i) {
        out[i] = {2.0, 0.0};
    }
    return out;
}

std::vector<std::complex<double>> freqz_ba_cpp(const IirFilter& filter,
                                               std::size_t wor_n,
                                               bool whole) {
    // DIVERGENCE: Python delegates this to scipy.signal.freqz() with
    // include_nyquist handling. The C++ port evaluates the same transfer
    // function directly on the matching frequency grid.
    const std::size_t count = wor_n;
    std::vector<std::complex<double>> out(count);
    const double step = whole ? (kTau / static_cast<double>(wor_n))
                              : ((count > 1U) ? (kPi / static_cast<double>(count - 1U)) : 0.0);
    for (std::size_t i = 0; i < count; ++i) {
        const double w = step * static_cast<double>(i);
        std::complex<double> z_inv = std::exp(std::complex<double>(0.0, -w));
        std::complex<double> num{0.0, 0.0};
        std::complex<double> den{0.0, 0.0};
        std::complex<double> z_pow{1.0, 0.0};
        for (double b : filter.b) {
            num += b * z_pow;
            z_pow *= z_inv;
        }
        z_pow = {1.0, 0.0};
        for (double a : filter.a) {
            den += a * z_pow;
            z_pow *= z_inv;
        }
        out[i] = num / den;
    }
    return out;
}

std::vector<std::complex<double>> sosfreqz_cpp(const SosFilter& filter,
                                               std::size_t wor_n,
                                               bool whole) {
    // DIVERGENCE: Python uses scipy.signal.sosfreqz(). The C++ port computes
    // the section product directly over the same sampled unit-circle grid.
    const std::size_t count = wor_n;
    std::vector<std::complex<double>> out(count, {1.0, 0.0});
    const double step = whole ? (kTau / static_cast<double>(wor_n))
                              : ((count > 1U) ? (kPi / static_cast<double>(count - 1U)) : 0.0);
    for (std::size_t i = 0; i < count; ++i) {
        const double w = step * static_cast<double>(i);
        std::complex<double> z_inv = std::exp(std::complex<double>(0.0, -w));
        for (const auto& sec : filter.sections) {
            std::complex<double> z2 = z_inv * z_inv;
            std::complex<double> num = sec.b0 + sec.b1 * z_inv + sec.b2 * z2;
            std::complex<double> den = sec.a0 + sec.a1 * z_inv + sec.a2 * z2;
            out[i] *= num / den;
        }
    }
    return out;
}

std::vector<std::complex<double>> zpk_freqz_cpp(const DigitalZpk& filt,
                                                std::size_t wor_n,
                                                bool whole) {
    const std::size_t count = wor_n;
    std::vector<std::complex<double>> out(count);
    const double step = whole ? (kTau / static_cast<double>(wor_n))
                              : ((count > 1U) ? (kPi / static_cast<double>(count - 1U)) : 0.0);
    for (std::size_t i = 0; i < count; ++i) {
        const double w = step * static_cast<double>(i);
        const std::complex<double> z = std::exp(std::complex<double>(0.0, w));
        std::complex<double> num = filt.k;
        for (const auto& zero : filt.z) num *= (z - zero);
        std::complex<double> den{1.0, 0.0};
        for (const auto& pole : filt.p) den *= (z - pole);
        out[i] = num / den;
    }
    return out;
}

std::vector<std::complex<double>> abs_values(const std::vector<std::complex<double>>& in) {
    std::vector<std::complex<double>> out(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        out[i] = {std::abs(in[i]), 0.0};
    }
    return out;
}

std::vector<std::complex<double>> multiply_complex(const std::vector<std::complex<double>>& a,
                                                   const std::vector<std::complex<double>>& b) {
    require(a.size() == b.size(), "multiply_complex size mismatch");
    std::vector<std::complex<double>> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[i] = a[i] * b[i];
    }
    return out;
}

std::vector<std::complex<double>> scale_complex(const std::vector<std::complex<double>>& a,
                                                std::complex<double> s) {
    std::vector<std::complex<double>> out(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        out[i] = a[i] * s;
    }
    return out;
}

std::vector<std::complex<double>> constant_complex(std::size_t n, std::complex<double> v) {
    return std::vector<std::complex<double>>(n, v);
}

double supergauss_value(double x, double freq, int order, double centerfreq = 0.0) {
    const double inner = (2.0 * (x - centerfreq) * std::pow(std::log(2.0) / 2.0, 1.0 / (2.0 * order))) /
                         freq;
    return std::exp(-2.0 * std::pow(inner, 2.0 * order));
}

std::vector<std::complex<double>> gen_video_lpf_supergauss_cpp(double corner_freq,
                                                               int order,
                                                               double nyquist_hz,
                                                               std::size_t blocklen) {
    const std::size_t bins = (blocklen / 2U) + 1U;
    std::vector<std::complex<double>> out(bins);
    for (std::size_t i = 0; i < bins; ++i) {
        double x = nyquist_hz * static_cast<double>(i) / static_cast<double>(bins - 1U);
        out[i] = {supergauss_value(x, corner_freq, order), 0.0};
    }
    return out;
}

std::vector<std::complex<double>> gen_bpf_supergauss_whole_cpp(double freq_low,
                                                               double freq_high,
                                                               int order,
                                                               double nyquist_hz,
                                                               std::size_t blocklen) {
    const std::size_t bins = (blocklen / 2U) + 1U;
    std::vector<double> half(bins);
    for (std::size_t i = 0; i < bins; ++i) {
        double x = nyquist_hz * static_cast<double>(i) / static_cast<double>(bins - 1U);
        half[i] = supergauss_value(x, freq_high - freq_low, order, (freq_high + freq_low) / 2.0);
    }

    std::vector<std::complex<double>> out(blocklen);
    for (std::size_t i = 0; i < (bins - 1U); ++i) {
        out[i] = {half[i], 0.0};
    }
    for (std::size_t i = 0; i < (bins - 1U); ++i) {
        out[bins - 1U + i] = {half[(bins - 2U) - i], 0.0};
    }
    return out;
}

std::pair<std::vector<double>, std::vector<double>> gen_shelf_cpp(double f0,
                                                                  double dbgain,
                                                                  const std::string& type,
                                                                  double fs,
                                                                  double qfactor) {
    const double a = std::pow(10.0, dbgain / 40.0);
    const double w0 = 2.0 * kPi * (f0 / fs);
    const double alpha = std::sin(w0) / (2.0 * qfactor);
    const double cosw0 = std::cos(w0);
    const double asquared = std::sqrt(a);

    double b0, b1, b2, a0, a1, a2;
    if (type == "low") {
        b0 = a * ((a + 1.0) - (a - 1.0) * cosw0 + 2.0 * asquared * alpha);
        b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosw0);
        b2 = a * ((a + 1.0) - (a - 1.0) * cosw0 - 2.0 * asquared * alpha);
        a0 = (a + 1.0) + (a - 1.0) * cosw0 + 2.0 * asquared * alpha;
        a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosw0);
        a2 = (a + 1.0) + (a - 1.0) * cosw0 - 2.0 * asquared * alpha;
    } else {
        b0 = a * ((a + 1.0) + (a - 1.0) * cosw0 + 2.0 * asquared * alpha);
        b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosw0);
        b2 = a * ((a + 1.0) + (a - 1.0) * cosw0 - 2.0 * asquared * alpha);
        a0 = (a + 1.0) - (a - 1.0) * cosw0 + 2.0 * asquared * alpha;
        a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosw0);
        a2 = (a + 1.0) - (a - 1.0) * cosw0 - 2.0 * asquared * alpha;
    }

    return {{b0, b1, b2}, {a0, a1, a2}};
}

IirFilter make_fm_deemphasis_b_cpp(double fs, double dbgain, double mid_point, double q) {
    auto coeffs = gen_shelf_cpp(mid_point, dbgain, "high", fs, q);
    // Python FMDeEmphasisB deliberately returns the shelf coefficients swapped:
    // FMDeEmphasisB.__init__ stores gen_shelf() into (ataps, btaps) and get()
    // returns (btaps, ataps). Mirror that exactly here.
    return {coeffs.second, coeffs.first};
}

std::vector<std::complex<double>> filtfft_cpp(const IirFilter& filt,
                                              std::size_t blocklen,
                                              bool whole) {
    if (filt.a.size() == 1U && std::abs(filt.a[0] - 1.0) < 1e-15) {
        std::vector<double> time(blocklen, 0.0);
        for (std::size_t i = 0; i < filt.b.size() && i < blocklen; ++i) time[i] = filt.b[i];
        fftw_complex* out = reinterpret_cast<fftw_complex*>(
            fftw_malloc(sizeof(fftw_complex) * ((blocklen / 2U) + 1U)));
        fftw_plan plan = fftw_plan_dft_r2c_1d(static_cast<int>(blocklen), time.data(), out, FFTW_ESTIMATE);
        fftw_execute(plan);
        std::vector<std::complex<double>> rv;
        if (whole) {
            rv.resize(blocklen);
            for (std::size_t i = 0; i <= (blocklen / 2U); ++i) {
                rv[i] = {out[i][0], out[i][1]};
            }
            for (std::size_t i = (blocklen / 2U) + 1U; i < blocklen; ++i) {
                const std::size_t src = blocklen - i;
                rv[i] = std::conj(rv[src]);
            }
        } else {
            rv.resize((blocklen / 2U) + 1U);
            for (std::size_t i = 0; i < rv.size(); ++i) {
                rv[i] = {out[i][0], out[i][1]};
            }
        }
        fftw_destroy_plan(plan);
        fftw_free(out);
        return rv;
    }
    const std::size_t wor_n = whole ? blocklen : ((blocklen / 2U) + 1U);
    return freqz_ba_cpp(filt, wor_n, whole);
}

std::complex<double> parse_python_complex(const std::string& text) {
    std::string s;
    for (char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) s.push_back(c);
    }
    require(!s.empty(), "empty complex filter line");
    require(s.back() == 'j', "custom filter line must end with 'j'");
    s.pop_back();

    std::size_t split = std::string::npos;
    for (std::size_t i = 1; i < s.size(); ++i) {
        if ((s[i] == '+' || s[i] == '-') && s[i - 1] != 'e' && s[i - 1] != 'E') {
            split = i;
        }
    }
    require(split != std::string::npos, "could not parse python complex literal");

    return {std::stod(s.substr(0, split)), std::stod(s.substr(split))};
}

std::vector<std::complex<double>> load_custom_filter_file_cpp(const std::filesystem::path& path) {
    std::ifstream in(path);
    require(in.is_open(), "unable to open custom video filter file");

    std::vector<std::complex<double>> values;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        values.push_back(parse_python_complex(line));
    }
    return values;
}

std::vector<std::complex<double>> gen_custom_video_filters_cpp(
    const std::vector<CustomVideoFilterSpec>& filter_list,
    double freq_hz,
    std::size_t blocklen) {
    std::vector<std::complex<double>> ret((blocklen / 2U) + 1U, {1.0, 0.0});
    for (const auto& f : filter_list) {
        switch (f.type) {
            case CustomVideoFilterType::File: {
                // DIVERGENCE: Python resolves package resources through
                // importlib.resources. The C++ port resolves the same files
                // directly from the checked-out tree relative to repo root.
                std::filesystem::path path = std::filesystem::path("vhsdecode/format_defs") /
                                             (f.filename + "-" + std::to_string(static_cast<int>(freq_hz)) + ".txt");
                auto loaded = load_custom_filter_file_cpp(path);
                require(loaded.size() == ret.size(), "custom filter file length mismatch");
                for (std::size_t i = 0; i < ret.size(); ++i) ret[i] *= loaded[i];
                break;
            }
            case CustomVideoFilterType::HighShelf:
            case CustomVideoFilterType::LowShelf: {
                auto coeffs = gen_shelf_cpp(
                    f.midfreq, f.gain,
                    f.type == CustomVideoFilterType::HighShelf ? "high" : "low",
                    freq_hz / 2.0, f.q);
                IirFilter shelf{coeffs.first, coeffs.second};
                auto shelf_fft = filtfft_cpp(shelf, blocklen, false);
                for (std::size_t i = 0; i < ret.size(); ++i) ret[i] *= shelf_fft[i];
                break;
            }
        }
    }
    return ret;
}

std::vector<double> firwin_lowpass_cpp(int numtaps, double cutoff_over_nyq) {
    // DIVERGENCE: Python uses scipy.signal.firwin(). The active K1 path only
    // needs a Hamming-window low-pass FIR here, so the C++ port implements
    // that specific case directly instead of porting SciPy's full firwin API.
    require((numtaps % 2) == 1, "firwin_lowpass_cpp expects odd numtaps");
    std::vector<double> taps(static_cast<std::size_t>(numtaps));
    const int m = numtaps - 1;
    const int center = m / 2;
    // SciPy firwin() takes cutoff normalized to Nyquist. The ideal low-pass
    // sinc formulation uses cutoff as a fraction of the full sample rate.
    const double fc = cutoff_over_nyq / 2.0;
    double sum = 0.0;
    for (int n = 0; n < numtaps; ++n) {
        const int k = n - center;
        const double sinc = (k == 0) ? (2.0 * fc) : (std::sin(kTau * fc * static_cast<double>(k)) /
                                                     (kPi * static_cast<double>(k)));
        const double window = 0.54 - 0.46 * std::cos(kTau * static_cast<double>(n) / static_cast<double>(m));
        taps[static_cast<std::size_t>(n)] = sinc * window;
        sum += taps[static_cast<std::size_t>(n)];
    }
    for (double& tap : taps) tap /= sum;
    return taps;
}

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
    out.z.reserve(in.z.size());
    out.p.reserve(in.p.size());
    for (const auto& z : in.z) out.z.push_back(z * wo);
    for (const auto& p : in.p) out.p.push_back(p * wo);
    const int degree = static_cast<int>(in.p.size()) - static_cast<int>(in.z.size());
    out.k = in.k * std::pow(wo, degree);
    return out;
}

DigitalZpk lp2hp_zpk_cpp(const DigitalZpk& in, double wo) {
    DigitalZpk out;
    out.z.reserve(in.z.size());
    out.p.reserve(in.p.size());
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

DigitalZpk lp2bp_zpk_cpp(const DigitalZpk& in, double wo, double bw) {
    DigitalZpk out;
    const int degree = static_cast<int>(in.p.size()) - static_cast<int>(in.z.size());
    out.z.reserve(in.z.size() * 2U + static_cast<std::size_t>(degree));
    out.p.reserve(in.p.size() * 2U);
    const double bw2 = bw / 2.0;
    for (const auto& z : in.z) {
        const auto temp = z * bw2;
        const auto rad = std::sqrt(temp * temp - std::complex<double>(wo * wo, 0.0));
        out.z.push_back(temp + rad);
        out.z.push_back(temp - rad);
    }
    for (const auto& p : in.p) {
        const auto temp = p * bw2;
        const auto rad = std::sqrt(temp * temp - std::complex<double>(wo * wo, 0.0));
        out.p.push_back(temp + rad);
        out.p.push_back(temp - rad);
    }
    for (int i = 0; i < degree; ++i) out.z.emplace_back(0.0, 0.0);
    out.k = in.k * std::pow(bw, degree);
    return out;
}

DigitalZpk bilinear_zpk_cpp(const DigitalZpk& in, double fs) {
    DigitalZpk out;
    out.z.reserve(in.z.size());
    out.p.reserve(in.p.size());
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

IirFilter zpk_to_tf_cpp(const DigitalZpk& zpk) {
    auto bpoly = poly_from_roots_cpp(zpk.z);
    auto apoly = poly_from_roots_cpp(zpk.p);
    for (auto& c : bpoly) c *= zpk.k;
    IirFilter out;
    out.b.resize(bpoly.size());
    out.a.resize(apoly.size());
    for (std::size_t i = 0; i < bpoly.size(); ++i) out.b[i] = bpoly[i].real();
    for (std::size_t i = 0; i < apoly.size(); ++i) out.a[i] = apoly[i].real();
    return out;
}

DigitalZpk ba_lp2lp_bilinear_cpp(const std::vector<double>& b,
                                 const std::vector<double>& a,
                                 double wn_norm,
                                 bool analog) {
    auto quad_roots = [](const std::vector<double>& c) {
        std::vector<std::complex<double>> roots;
        if (c.size() == 3U) {
            const std::complex<double> disc =
                std::complex<double>(c[1] * c[1] - 4.0 * c[0] * c[2], 0.0);
            const auto sdisc = std::sqrt(disc);
            roots.push_back((-c[1] + sdisc) / (2.0 * c[0]));
            roots.push_back((-c[1] - sdisc) / (2.0 * c[0]));
        } else if (c.size() == 2U) {
            roots.push_back(std::complex<double>(-c[1] / c[0], 0.0));
        }
        return roots;
    };

    DigitalZpk zpk;
    zpk.z = quad_roots(b);
    zpk.p = quad_roots(a);
    zpk.k = std::complex<double>(b.empty() ? 1.0 : b[0], 0.0) /
            std::complex<double>(a.empty() ? 1.0 : a[0], 0.0);

    double warped = wn_norm;
    if (!analog) {
        const double fs = 2.0;
        warped = 2.0 * fs * std::tan(kPi * wn_norm / fs);
    }
    return analog ? lp2lp_zpk_cpp(zpk, warped) : bilinear_zpk_cpp(lp2lp_zpk_cpp(zpk, warped), 2.0);
}

std::vector<std::complex<double>> peaking_freq_response_cpp(double wn_norm,
                                                            double dbgain,
                                                            std::optional<double> q,
                                                            std::optional<double> bw,
                                                            const std::string& type,
                                                            std::size_t blocklen,
                                                            bool whole) {
    double q_value = 0.0;
    if (q.has_value()) {
        q_value = *q;
    } else if (bw.has_value()) {
        q_value = 1.0 / (2.0 * std::sinh((std::log(2.0) / 2.0) * *bw));
    } else {
        q_value = 1.0 / (2.0 * std::sinh((std::log(2.0) / 2.0) * 1.0));
    }

    if (type == "half") {
        const double A = std::pow(10.0, dbgain / 40.0);
        const double Az = A;
        const double Ap = A;
        const std::vector<double> b{1.0, Az / q_value, 1.0};
        const std::vector<double> a{1.0, 1.0 / (Ap * q_value), 1.0};
        return zpk_freqz_cpp(ba_lp2lp_bilinear_cpp(b, a, wn_norm, false), whole ? blocklen : ((blocklen / 2U) + 1U), whole);
    } else if (type == "constantq") {
        const double A = std::pow(10.0, dbgain / 20.0);
        double Az = 0.0;
        double Ap = 0.0;
        if (dbgain > 0.0) {
            Az = A;
            Ap = 1.0;
        } else {
            Az = 1.0;
            Ap = A;
        }
        const std::vector<double> b{1.0, Az / q_value, 1.0};
        const std::vector<double> a{1.0, 1.0 / (Ap * q_value), 1.0};
        return zpk_freqz_cpp(ba_lp2lp_bilinear_cpp(b, a, wn_norm, false), whole ? blocklen : ((blocklen / 2U) + 1U), whole);
    } else {
        throw std::runtime_error("unsupported peaking type");
    }
}

DigitalZpk butter_digital_lowpass_zpk_cpp(int order, double cutoff_hz, double fs) {
    return bilinear_zpk_cpp(lp2lp_zpk_cpp(buttap_zpk_cpp(order), 2.0 * fs * std::tan(kPi * cutoff_hz / fs)), fs);
}

DigitalZpk butter_digital_highpass_zpk_cpp(int order, double cutoff_hz, double fs) {
    return bilinear_zpk_cpp(lp2hp_zpk_cpp(buttap_zpk_cpp(order), 2.0 * fs * std::tan(kPi * cutoff_hz / fs)), fs);
}

DigitalZpk butter_digital_bandpass_zpk_cpp(int order, double low_hz, double high_hz, double fs) {
    const double warped_low = 2.0 * fs * std::tan(kPi * low_hz / fs);
    const double warped_high = 2.0 * fs * std::tan(kPi * high_hz / fs);
    const double bw = warped_high - warped_low;
    const double wo = std::sqrt(warped_low * warped_high);
    return bilinear_zpk_cpp(lp2bp_zpk_cpp(buttap_zpk_cpp(order), wo, bw), fs);
}

std::vector<double> butterworth_qs(int order) {
    std::vector<double> qs;
    for (int k = 1; k <= order / 2; ++k) {
        qs.push_back(1.0 / (2.0 * std::cos(kPi * (2.0 * static_cast<double>(k) - 1.0) /
                                           (2.0 * static_cast<double>(order)))));
    }
    return qs;
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

SosSection lowpass_biquad_cpp(double fs, double cutoff_hz, double q) {
    // DIVERGENCE: SciPy's butter()/sos output comes from analog-prototype
    // transforms. This local helper uses the standard RBJ bilinear biquad form
    // to synthesize an equivalent low-pass section for the C++ port.
    const double w0 = kTau * cutoff_hz / fs;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    SosSection s;
    s.b0 = (1.0 - cosw0) / 2.0;
    s.b1 = 1.0 - cosw0;
    s.b2 = (1.0 - cosw0) / 2.0;
    s.a0 = 1.0 + alpha;
    s.a1 = -2.0 * cosw0;
    s.a2 = 1.0 - alpha;
    return s;
}

SosSection lowpass_first_order_sos_cpp(double fs, double cutoff_hz) {
    IirFilter f = lowpass_first_order_cpp(fs, cutoff_hz);
    return {f.b[0], f.b[1], 0.0, f.a[0], f.a[1], 0.0};
}

SosSection highpass_biquad_cpp(double fs, double cutoff_hz, double q) {
    // DIVERGENCE: See lowpass_biquad_cpp() above; this is the matching
    // high-pass RBJ-style section used in place of scipy.signal.butter().
    const double w0 = kTau * cutoff_hz / fs;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    SosSection s;
    s.b0 = (1.0 + cosw0) / 2.0;
    s.b1 = -(1.0 + cosw0);
    s.b2 = (1.0 + cosw0) / 2.0;
    s.a0 = 1.0 + alpha;
    s.a1 = -2.0 * cosw0;
    s.a2 = 1.0 - alpha;
    return s;
}

SosSection highpass_first_order_sos_cpp(double fs, double cutoff_hz) {
    IirFilter f = highpass_first_order_cpp(fs, cutoff_hz);
    return {f.b[0], f.b[1], 0.0, f.a[0], f.a[1], 0.0};
}

SosSection bandpass_biquad_cpp(double fs, double low_hz, double high_hz, double q_scale) {
    // DIVERGENCE: SciPy's band-pass Butterworth design is replaced here with a
    // cascaded biquad approximation using the same center frequency and
    // Butterworth-derived section Qs.
    const double center_hz = std::sqrt(low_hz * high_hz);
    const double bandwidth_hz = high_hz - low_hz;
    const double q = (center_hz / bandwidth_hz) * q_scale;
    const double w0 = kTau * center_hz / fs;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    SosSection s;
    s.b0 = alpha;
    s.b1 = 0.0;
    s.b2 = -alpha;
    s.a0 = 1.0 + alpha;
    s.a1 = -2.0 * cosw0;
    s.a2 = 1.0 - alpha;
    return s;
}

SosFilter make_butter_lowpass_sos_cpp(int order, double cutoff_hz, double fs) {
    SosFilter out;
    if (order % 2) {
        out.sections.push_back(lowpass_first_order_sos_cpp(fs, cutoff_hz));
    }
    for (double q : butterworth_qs(order)) {
        out.sections.push_back(lowpass_biquad_cpp(fs, cutoff_hz, q));
    }
    return out;
}

SosFilter make_butter_highpass_sos_cpp(int order, double cutoff_hz, double fs) {
    SosFilter out;
    if (order % 2) {
        out.sections.push_back(highpass_first_order_sos_cpp(fs, cutoff_hz));
    }
    for (double q : butterworth_qs(order)) {
        out.sections.push_back(highpass_biquad_cpp(fs, cutoff_hz, q));
    }
    return out;
}

SosFilter make_butter_bandpass_sos_cpp(int order, double low_hz, double high_hz, double fs) {
    SosFilter out;
    if (order == 1) {
        out.sections.push_back(bandpass_biquad_cpp(fs, low_hz, high_hz, 1.0));
        return out;
    }
    if (order % 2) {
        // DIVERGENCE: SciPy supports odd-order Butterworth band-pass design.
        // The C++ port currently approximates only the even-order/first-order
        // cases used by the active NTSC VHS K1 path.
        throw std::runtime_error("odd-order bandpass SOS not supported in local builder");
    }
    for (double q : butterworth_qs(order)) {
        out.sections.push_back(bandpass_biquad_cpp(fs, low_hz, high_hz, q));
    }
    return out;
}

IirFilter iirnotch_cpp(double w0_norm_nyq, double q) {
    const double w0 = kPi * w0_norm_nyq;
    const double alpha = std::sin(w0) / (2.0 * q);
    double b0 = 1.0;
    double b1 = -2.0 * std::cos(w0);
    double b2 = 1.0;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * std::cos(w0);
    double a2 = 1.0 - alpha;
    return {{b0 / a0, b1 / a0, b2 / a0}, {1.0, a1 / a0, a2 / a0}};
}

std::pair<int, double> buttord_cpp(double wp_hz,
                                   double ws_hz,
                                   double gpass_db,
                                   double gstop_db,
                                   double fs_hz) {
    // DIVERGENCE: SciPy's buttord() has detailed numeric handling and broader
    // band-type coverage. This local port mirrors the standard digital
    // Butterworth low/high-pass derivation used by K1's VideoEQ path.
    const double wp = 2.0 * fs_hz * std::tan(kPi * wp_hz / fs_hz);
    const double ws = 2.0 * fs_hz * std::tan(kPi * ws_hz / fs_hz);
    const double gp = std::pow(10.0, gpass_db / 10.0) - 1.0;
    const double gs = std::pow(10.0, gstop_db / 10.0) - 1.0;
    const double ratio = std::max(ws, wp) / std::min(ws, wp);
    const int order = static_cast<int>(std::ceil(std::log10(gs / gp) / (2.0 * std::log10(ratio))));
    return {std::max(order, 1), wp_hz};
}

IirFilter firdes_highpass_cpp(double samp_rate, double cutoff, double transition_width, int order_limit) {
    const auto [order_raw, normal_cutoff] = buttord_cpp(cutoff, cutoff + transition_width, 3.0, 30.0, samp_rate);
    const int order = std::min(order_raw, order_limit);
    return zpk_to_tf_cpp(butter_digital_highpass_zpk_cpp(order, normal_cutoff, samp_rate));
}

std::vector<std::complex<double>> gen_video_lpf_params_cpp(const K1DecoderParams& dp,
                                                           double nyquist_hz,
                                                           std::size_t blocklen) {
    if (dp.video_lpf_supergauss) {
        return gen_video_lpf_supergauss_cpp(dp.video_lpf_freq, dp.video_lpf_order, nyquist_hz, blocklen);
    }
    auto zpk = butter_digital_lowpass_zpk_cpp(dp.video_lpf_order, dp.video_lpf_freq, nyquist_hz * 2.0);
    auto resp = zpk_freqz_cpp(zpk, blocklen, true);
    resp.resize((blocklen / 2U) + 1U);
    return abs_values(resp);
}

std::vector<std::complex<double>> gen_nonlinear_bandpass_params_cpp(const K1DecoderParams& dp,
                                                                    double nyquist_hz,
                                                                    std::size_t blocklen) {
    const double fs = nyquist_hz * 2.0;
    if (dp.nonlinear_bandpass_upper.has_value()) {
        auto zpk = butter_digital_bandpass_zpk_cpp(dp.nonlinear_bandpass_order,
                                                   dp.nonlinear_highpass_freq,
                                                   *dp.nonlinear_bandpass_upper,
                                                   fs);
        return abs_values(zpk_freqz_cpp(zpk, (blocklen / 2U) + 1U, false));
    }
    return zpk_freqz_cpp(
        butter_digital_highpass_zpk_cpp(dp.nonlinear_bandpass_order, dp.nonlinear_highpass_freq, fs),
        (blocklen / 2U) + 1U,
        false);
}

std::vector<std::complex<double>> gen_fm_audio_notch_params_cpp(const K1DecoderParams& dp,
                                                                int notch_q,
                                                                double nyquist_hz,
                                                                std::size_t blocklen) {
    require(dp.fm_audio_channel_0_freq.has_value() && dp.fm_audio_channel_1_freq.has_value(),
            "fm audio notch requires both audio channel frequencies");
    auto notch0 = filtfft_cpp(iirnotch_cpp(*dp.fm_audio_channel_0_freq / nyquist_hz, static_cast<double>(notch_q)),
                              blocklen, true);
    auto notch1 = filtfft_cpp(iirnotch_cpp(*dp.fm_audio_channel_1_freq / nyquist_hz, static_cast<double>(notch_q)),
                              blocklen, true);
    return multiply_complex(notch0, notch1);
}

std::vector<std::complex<double>> gen_ramp_filter_params_cpp(const K1DecoderParams& dp,
                                                             double nyquist_hz,
                                                             std::size_t blocklen) {
    const double start_freq_hz = dp.start_rf_linear.value_or(0.0);
    const double boost_start = dp.boost_rf_linear_0.value_or(0.0);
    const double boost_max = dp.boost_rf_linear_20;
    const double max_freq_hz = 20e6;
    const std::size_t half = blocklen / 2U;
    const std::size_t zero_ratio = static_cast<std::size_t>((start_freq_hz / nyquist_hz) * static_cast<double>(half));

    std::vector<std::complex<double>> out(blocklen);
    for (std::size_t i = 0; i < half; ++i) {
        double v = 0.0;
        if (i >= zero_ratio && half > zero_ratio) {
            double t = static_cast<double>(i - zero_ratio) / static_cast<double>(half - zero_ratio);
            v = boost_start + t * ((boost_max * (nyquist_hz / max_freq_hz)) - boost_start);
        }
        out[i] = {v, 0.0};
        out[blocklen - 1U - i] = {v, 0.0};
    }
    return out;
}

VideoEqConfig build_video_eq_config_cpp(const K1BuildConfig& config) {
    require(config.decoder_params.video_eq.has_value(),
            "video_eq requested without decoder video_eq params");
    const auto& veq = *config.decoder_params.video_eq;
    IirFilter filt = firdes_highpass_cpp(config.freq_hz, veq.corner, veq.transition, veq.order_limit);
    return {filt, static_cast<double>(veq.order_limit), config.runtime.sharpness_level};
}

SubDeemphasisConfig build_subdeemph_config_cpp(const K1BuildConfig& config) {
    const auto& dp = config.decoder_params;
    SubDeemphasisConfig out;
    out.exponential_scaling = dp.nonlinear_exp_scaling;
    out.scaling_1 = dp.nonlinear_scaling_1;
    out.scaling_2 = dp.nonlinear_scaling_2;
    out.logistic_mid = dp.nonlinear_logistic_mid.value_or(0.0);
    out.logistic_rate = dp.nonlinear_logistic_rate.value_or(0.0);
    out.static_factor = dp.nonlinear_static_factor;
    out.deviation = dp.nonlinear_deviation.value_or(config.hz_ire * (100.0 + -config.vsync_ire));
    return out;
}

}  // namespace

K1Context build_k1_context(const K1BuildConfig& config) {
    require(config.blocklen > 0, "build_k1_context requires non-zero blocklen");
    require((config.blocklen % 2U) == 0U, "build_k1_context requires even blocklen");
    require(config.freq_hz > 0.0, "build_k1_context requires positive sample rate");

    const auto& dp = config.decoder_params;
    const auto& rt = config.runtime;
    const double freq_hz_half = config.freq_hz / 2.0;

    K1Context ctx;
    ctx.filters.blocklen = config.blocklen;
    ctx.filters.f05_offset = 32;
    ctx.filters.hilbert = build_hilbert_cpp(config.blocklen);

    if (dp.video_bpf_supergauss) {
        ctx.filters.rf_video = gen_bpf_supergauss_whole_cpp(
            dp.video_bpf_low, dp.video_bpf_high, dp.video_bpf_order, freq_hz_half, config.blocklen);
    } else {
        std::vector<std::complex<double>> rf_video(config.blocklen, {1.0, 0.0});

        if (dp.video_bpf_order > 0) {
            ctx.debug.rf_bpf = abs_values(zpk_freqz_cpp(
                butter_digital_bandpass_zpk_cpp(dp.video_bpf_order,
                                                dp.video_bpf_low,
                                                dp.video_bpf_high,
                                                config.freq_hz),
                config.blocklen,
                true));
            rf_video = ctx.debug.rf_bpf;
        }

        ctx.debug.rf_extra_lpf = abs_values(zpk_freqz_cpp(
            butter_digital_lowpass_zpk_cpp(dp.video_lpf_extra_order, dp.video_lpf_extra, config.freq_hz),
            config.blocklen,
            true));
        ctx.debug.rf_extra_hpf = abs_values(zpk_freqz_cpp(
            butter_digital_highpass_zpk_cpp(dp.video_hpf_extra_order, dp.video_hpf_extra, config.freq_hz),
            config.blocklen,
            true));
        rf_video = multiply_complex(rf_video, ctx.debug.rf_extra_lpf);
        rf_video = multiply_complex(rf_video, ctx.debug.rf_extra_hpf);
        ctx.filters.rf_video = rf_video;
    }

    if (dp.video_rf_peak_freq.has_value()) {
        ctx.debug.rf_peak = abs_values(peaking_freq_response_cpp(
            *dp.video_rf_peak_freq / freq_hz_half,
            dp.video_rf_peak_gain,
            std::nullopt,
            dp.video_rf_peak_bandwidth / freq_hz_half,
            "constantq",
            config.blocklen,
            true));
        ctx.filters.rf_video = multiply_complex(ctx.filters.rf_video, ctx.debug.rf_peak);
    }

    if (rt.fm_audio_notch_q > 0 &&
        dp.fm_audio_channel_0_freq.has_value() &&
        dp.fm_audio_channel_1_freq.has_value()) {
        ctx.debug.rf_audio_notch = abs_values(
            gen_fm_audio_notch_params_cpp(dp, rt.fm_audio_notch_q, freq_hz_half, config.blocklen));
        ctx.filters.rf_video = multiply_complex(ctx.filters.rf_video, ctx.debug.rf_audio_notch);
    }

    if (dp.boost_rf_linear_0.has_value()) {
        ctx.debug.rf_ramp = gen_ramp_filter_params_cpp(dp, freq_hz_half, config.blocklen);
        ctx.filters.rf_video = multiply_complex(ctx.filters.rf_video, ctx.debug.rf_ramp);
        if (dp.boost_rf_linear_double) {
            ctx.filters.rf_video = multiply_complex(ctx.filters.rf_video, ctx.debug.rf_ramp);
        }
    }

    ctx.filters.rf_top = make_butter_bandpass_sos_cpp(1, dp.boost_bpf_low, dp.boost_bpf_high, config.freq_hz);

    IirFilter deemph = make_fm_deemphasis_b_cpp(config.freq_hz, dp.deemph_gain, dp.deemph_mid, dp.deemph_q);
    auto filter_deemp = filtfft_cpp(deemph, config.blocklen, false);
    auto filter_video_lpf = gen_video_lpf_params_cpp(dp, freq_hz_half, config.blocklen);
    auto filter_custom = dp.video_custom_luma_filters.empty()
                             ? constant_complex((config.blocklen / 2U) + 1U, {1.0, 0.0})
                             : gen_custom_video_filters_cpp(dp.video_custom_luma_filters, config.freq_hz, config.blocklen);

    IirFilter filter_05{{}, {}};
    filter_05.b = firwin_lowpass_cpp(65, 0.5e6 / freq_hz_half);
    filter_05.a = {1.0};
    auto filter_05_fft = filtfft_cpp(filter_05, config.blocklen, false);
    ctx.debug.filter_deemp = filter_deemp;
    ctx.debug.filter_video_lpf = filter_video_lpf;
    ctx.debug.filter_custom = filter_custom;
    ctx.debug.filter_05 = filter_05_fft;
    ctx.debug.filter_05_taps = filter_05.b;

    ctx.filters.fenv_post = SosFilter{{lowpass_first_order_sos_cpp(config.freq_hz, 700000.0)}};
    ctx.filters.nl_amplitude_lpf = SosFilter{{
        lowpass_first_order_sos_cpp(config.freq_hz, dp.nonlinear_amp_lpf_freq)
    }};

    ctx.filters.fvideo = multiply_complex(multiply_complex(filter_deemp, filter_video_lpf), filter_custom);
    ctx.filters.fvideo05 = multiply_complex(multiply_complex(filter_video_lpf, filter_deemp), filter_05_fft);

    if (rt.enable_nldeemp || rt.enable_subdeemp) {
        ctx.filters.nl_highpass_f = gen_nonlinear_bandpass_params_cpp(dp, freq_hz_half, config.blocklen);
    }

    if (rt.enable_video_notch && rt.video_notch_freq > 0.0) {
        double notch_den = rt.do_cafc && rt.cafc_output_freq_half_hz.has_value()
                               ? *rt.cafc_output_freq_half_hz
                               : freq_hz_half;
        ctx.filters.fvideo_notch = iirnotch_cpp(rt.video_notch_freq / notch_den, rt.video_notch_q);
        ctx.filters.fvideo_notch_f = abs_values(filtfft_cpp(
            iirnotch_cpp(rt.video_notch_freq / freq_hz_half, rt.video_notch_q),
            config.blocklen,
            true));
    }

    if (rt.enable_chroma_audio_notch && dp.chroma_audio_notch_freq.has_value()) {
        double notch_den = rt.do_cafc && rt.cafc_output_freq_half_hz.has_value()
                               ? *rt.cafc_output_freq_half_hz
                               : freq_hz_half;
        ctx.filters.chroma_audio_notch = iirnotch_cpp(*dp.chroma_audio_notch_freq / notch_den, 10.0);
    }

    if (rt.enable_fsc_notch) {
        ctx.filters.fsc_notch = iirnotch_cpp(config.fsc_mhz / (freq_hz_half / 1e6), 2.0);
    }

    ctx.chroma_afc.emplace(ChromaAfcConfig{
        config.freq_hz,
        dp.chroma_bpf_upper / dp.color_under_carrier,
        config.fps,
        config.frame_lines,
        config.max_field_lines,
        config.outlinelen,
        config.fsc_mhz,
        dp.color_under_carrier,
        dp.chroma_bpf_order,
        false,
        rt.do_cafc,
        dp.chroma_bpf_lower,
        config.tape_format,
    });
    if (rt.color_under) {
        ctx.filters.fvideo_burst = ctx.chroma_afc->getChromaBandpass();
    } else {
        ctx.filters.fvideo_burst = ctx.chroma_afc->getChromaBandpassFinal(false);
    }

    ctx.options.enable_video_notch = rt.enable_video_notch;
    ctx.options.enable_high_boost = rt.high_boost.has_value();
    ctx.options.high_boost = rt.high_boost.value_or(dp.boost_bpf_mult);
    ctx.options.disable_diff_demod = rt.disable_diff_demod;
    ctx.options.diff_demod_check_value = rt.diff_demod_check_value;

    ctx.options.enable_video_eq = (rt.sharpness_level != 0.0) && dp.video_eq.has_value();
    if (ctx.options.enable_video_eq) {
        ctx.options.video_eq = build_video_eq_config_cpp(config);
    }

    ctx.options.enable_chroma_trap = rt.enable_chroma_trap;
    if (ctx.options.enable_chroma_trap) {
        ctx.options.chroma_trap = ChromaTrapConfig{config.freq_hz, config.fsc_mhz, 8, 4};
    }

    ctx.options.enable_nldeemp = rt.enable_nldeemp;
    if (ctx.options.enable_nldeemp) {
        ctx.options.nonlinear_deemphasis = NonLinearDeemphasisConfig{
            dp.nonlinear_highpass_limit_l,
            dp.nonlinear_highpass_limit_h,
        };
    }

    ctx.options.enable_subdeemp = rt.enable_subdeemp && dp.use_sub_deemphasis;
    if (ctx.options.enable_subdeemp) {
        ctx.options.sub_deemphasis = build_subdeemph_config_cpp(config);
    }

    ctx.options.enable_fsc_notch = rt.enable_fsc_notch;
    ctx.options.color_under = rt.color_under;
    ctx.options.chroma_offset = rt.chroma_offset;

    return ctx;
}

}  // namespace vhsdecode::cppport

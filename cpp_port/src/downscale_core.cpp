#include "vhsdecode_cpp/downscale_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace vhsdecode_cpp {
namespace {

void require(bool cond, const char* msg) {
    if (!cond) throw std::runtime_error(msg);
}

double median_copy(std::vector<double> v) {
    require(!v.empty(), "median on empty vector");
    const std::size_t mid = v.size() / 2U;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    if ((v.size() & 1U) != 0U) return v[mid];
    const double hi = v[mid];
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid - 1U), v.begin() + static_cast<std::ptrdiff_t>(mid));
    return 0.5 * (v[mid - 1U] + hi);
}

double eval_linear_spline(const std::vector<double>& x, const std::vector<double>& y, double xi) {
    if (xi <= x.front()) {
        const double slope = (y[1] - y[0]) / (x[1] - x[0]);
        return y[0] + (xi - x[0]) * slope;
    }
    if (xi >= x.back()) {
        const std::size_t n = x.size();
        const double slope = (y[n - 1] - y[n - 2]) / (x[n - 1] - x[n - 2]);
        return y[n - 1] + (xi - x[n - 1]) * slope;
    }
    auto it = std::upper_bound(x.begin(), x.end(), xi);
    const std::size_t hi = static_cast<std::size_t>(std::distance(x.begin(), it));
    const std::size_t lo = hi - 1U;
    const double t = (xi - x[lo]) / (x[hi] - x[lo]);
    return y[lo] + t * (y[hi] - y[lo]);
}

double eval_linear_spline_derivative(const std::vector<double>& x, const std::vector<double>& y, double xi) {
    if (xi <= x.front()) return (y[1] - y[0]) / (x[1] - x[0]);
    if (xi >= x.back()) {
        const std::size_t n = x.size();
        return (y[n - 1] - y[n - 2]) / (x[n - 1] - x[n - 2]);
    }
    auto it = std::upper_bound(x.begin(), x.end(), xi);
    const std::size_t hi = static_cast<std::size_t>(std::distance(x.begin(), it));
    const std::size_t lo = hi - 1U;
    return (y[hi] - y[lo]) / (x[hi] - x[lo]);
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
        require(best > 1e-18, "singular spline linear system");
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

std::vector<double> build_not_a_knot(const std::vector<double>& x, int k) {
    require(!x.empty(), "empty x for spline knots");
    std::vector<double> t_inner;
    if ((k & 1) != 0) {
        t_inner = x;
        const int k2 = (k + 1) / 2;
        t_inner = std::vector<double>(t_inner.begin() + k2, t_inner.end() - k2);
    } else {
        t_inner.resize(x.size() - 1U);
        for (std::size_t i = 0; i + 1 < x.size(); ++i) t_inner[i] = 0.5 * (x[i] + x[i + 1]);
        const int k2 = k / 2;
        t_inner = std::vector<double>(t_inner.begin() + k2, t_inner.end() - k2);
    }
    std::vector<double> t;
    t.reserve(static_cast<std::size_t>(k + 1) + t_inner.size() + static_cast<std::size_t>(k + 1));
    for (int i = 0; i < k + 1; ++i) t.push_back(x.front());
    t.insert(t.end(), t_inner.begin(), t_inner.end());
    for (int i = 0; i < k + 1; ++i) t.push_back(x.back());
    return t;
}

std::vector<double> build_augknt(const std::vector<double>& x, int k) {
    std::vector<double> t;
    t.reserve(static_cast<std::size_t>(k) + x.size() + static_cast<std::size_t>(k));
    for (int i = 0; i < k; ++i) t.push_back(x.front());
    t.insert(t.end(), x.begin(), x.end());
    for (int i = 0; i < k; ++i) t.push_back(x.back());
    return t;
}

int find_span(const std::vector<double>& t, int degree, double x) {
    const int n = static_cast<int>(t.size()) - degree - 2;
    if (x >= t[static_cast<std::size_t>(n + 1)]) return n;
    if (x <= t[static_cast<std::size_t>(degree)]) return degree;
    int low = degree;
    int high = n + 1;
    int mid = (low + high) / 2;
    while (x < t[static_cast<std::size_t>(mid)] || x >= t[static_cast<std::size_t>(mid + 1)]) {
        if (x < t[static_cast<std::size_t>(mid)]) high = mid;
        else low = mid;
        mid = (low + high) / 2;
    }
    return mid;
}

std::vector<double> ders_basis_funs(int span, double x, int degree, int n_deriv, const std::vector<double>& t) {
    std::vector<double> ndu(static_cast<std::size_t>((degree + 1) * (degree + 1)), 0.0);
    std::vector<double> left(static_cast<std::size_t>(degree + 1), 0.0);
    std::vector<double> right(static_cast<std::size_t>(degree + 1), 0.0);
    auto idx = [degree](int i, int j) { return static_cast<std::size_t>(i * (degree + 1) + j); };
    ndu[idx(0, 0)] = 1.0;
    for (int j = 1; j <= degree; ++j) {
        left[static_cast<std::size_t>(j)] = x - t[static_cast<std::size_t>(span + 1 - j)];
        right[static_cast<std::size_t>(j)] = t[static_cast<std::size_t>(span + j)] - x;
        double saved = 0.0;
        for (int r = 0; r < j; ++r) {
            ndu[idx(j, r)] = right[static_cast<std::size_t>(r + 1)] + left[static_cast<std::size_t>(j - r)];
            double temp = 0.0;
            if (ndu[idx(j, r)] != 0.0) temp = ndu[idx(r, j - 1)] / ndu[idx(j, r)];
            ndu[idx(r, j)] = saved + right[static_cast<std::size_t>(r + 1)] * temp;
            saved = left[static_cast<std::size_t>(j - r)] * temp;
        }
        ndu[idx(j, j)] = saved;
    }
    std::vector<double> ders(static_cast<std::size_t>((n_deriv + 1) * (degree + 1)), 0.0);
    auto didx = [degree](int k, int j) { return static_cast<std::size_t>(k * (degree + 1) + j); };
    for (int j = 0; j <= degree; ++j) ders[didx(0, j)] = ndu[idx(j, degree)];

    std::vector<double> a(static_cast<std::size_t>(2 * (degree + 1)), 0.0);
    auto aidx = [degree](int row, int col) { return static_cast<std::size_t>(row * (degree + 1) + col); };
    for (int r = 0; r <= degree; ++r) {
        int s1 = 0;
        int s2 = 1;
        a[aidx(0, 0)] = 1.0;
        for (int k = 1; k <= n_deriv; ++k) {
            double d = 0.0;
            const int rk = r - k;
            const int pk = degree - k;
            int j1, j2;
            if (r >= k) {
                a[aidx(s2, 0)] = a[aidx(s1, 0)] / ndu[idx(pk + 1, rk)];
                d = a[aidx(s2, 0)] * ndu[idx(rk, pk)];
            }
            if (rk >= -1) j1 = 1;
            else j1 = -rk;
            if (r - 1 <= pk) j2 = k - 1;
            else j2 = degree - r;
            for (int j = j1; j <= j2; ++j) {
                a[aidx(s2, j)] = (a[aidx(s1, j)] - a[aidx(s1, j - 1)]) / ndu[idx(pk + 1, rk + j)];
                d += a[aidx(s2, j)] * ndu[idx(rk + j, pk)];
            }
            if (r <= pk) {
                a[aidx(s2, k)] = -a[aidx(s1, k - 1)] / ndu[idx(pk + 1, r)];
                d += a[aidx(s2, k)] * ndu[idx(r, pk)];
            }
            ders[didx(k, r)] = d;
            std::swap(s1, s2);
        }
    }
    int r = degree;
    for (int k = 1; k <= n_deriv; ++k) {
        for (int j = 0; j <= degree; ++j) ders[didx(k, j)] *= static_cast<double>(r);
        r *= (degree - k);
    }
    return ders;
}

std::vector<double> build_spline_coeffs(const std::vector<double>& x, const std::vector<double>& y, int degree) {
    require(x.size() == y.size(), "spline x/y mismatch");
    require(!x.empty(), "empty spline data");
    const std::vector<double> t = (degree == 2) ? build_not_a_knot(x, degree) : build_augknt(x, degree);
    const int n = static_cast<int>(x.size());
    const int nt = static_cast<int>(t.size()) - degree - 1;
    std::vector<double> a(static_cast<std::size_t>(nt * nt), 0.0);
    std::vector<double> b(static_cast<std::size_t>(nt), 0.0);
    auto aidx = [nt](int row, int col) { return static_cast<std::size_t>(row * nt + col); };

    int row = 0;
    if (degree == 3) {
        const int span0 = find_span(t, degree, x.front());
        const auto ders0 = ders_basis_funs(span0, x.front(), degree, 2, t);
        for (int j = 0; j <= degree; ++j) {
            const int col = span0 - degree + j;
            if (col >= 0 && col < nt) a[aidx(row, col)] = ders0[static_cast<std::size_t>(2 * (degree + 1) + j)];
        }
        b[static_cast<std::size_t>(row)] = 0.0;
        ++row;
    }
    for (int i = 0; i < n; ++i) {
        const int span = find_span(t, degree, x[static_cast<std::size_t>(i)]);
        const auto ders = ders_basis_funs(span, x[static_cast<std::size_t>(i)], degree, 0, t);
        for (int j = 0; j <= degree; ++j) {
            const int col = span - degree + j;
            if (col >= 0 && col < nt) a[aidx(row, col)] = ders[static_cast<std::size_t>(j)];
        }
        b[static_cast<std::size_t>(row)] = y[static_cast<std::size_t>(i)];
        ++row;
    }
    if (degree == 3) {
        const int span1 = find_span(t, degree, x.back());
        const auto ders1 = ders_basis_funs(span1, x.back(), degree, 2, t);
        for (int j = 0; j <= degree; ++j) {
            const int col = span1 - degree + j;
            if (col >= 0 && col < nt) a[aidx(row, col)] = ders1[static_cast<std::size_t>(2 * (degree + 1) + j)];
        }
        b[static_cast<std::size_t>(row)] = 0.0;
    }
    return solve_linear_system(std::move(a), std::move(b), nt);
}

std::pair<double, double> eval_bspline_and_derivative(const std::vector<double>& t,
                                                      const std::vector<double>& c,
                                                      int degree,
                                                      double x) {
    const int span = find_span(t, degree, x);
    const auto ders = ders_basis_funs(span, x, degree, 1, t);
    double value = 0.0;
    double deriv = 0.0;
    for (int j = 0; j <= degree; ++j) {
        const int col = span - degree + j;
        if (col >= 0 && static_cast<std::size_t>(col) < c.size()) {
            value += c[static_cast<std::size_t>(col)] * ders[static_cast<std::size_t>(j)];
            deriv += c[static_cast<std::size_t>(col)] * ders[static_cast<std::size_t>((degree + 1) + j)];
        }
    }
    return {value, deriv};
}

std::vector<float> y_comb_cpp(std::vector<float> data, int line_len, double limit) {
    const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(data.size());
    for (std::ptrdiff_t i = 0; i < n; ++i) {
        const std::ptrdiff_t back = (i - line_len + n) % n;
        const std::ptrdiff_t fwd = (i + line_len) % n;
        const double diffb = static_cast<double>(data[i]) - static_cast<double>(data[fwd]);
        const double difff = static_cast<double>(data[i]) - static_cast<double>(data[back]);
        const double corr = std::clamp(diffb + difff, -limit, limit) / 2.0;
        data[i] = static_cast<float>(static_cast<double>(data[i]) - corr);
    }
    return data;
}

std::vector<std::uint16_t> hz_to_output_array_cpp(const std::vector<float>& input,
                                                  double ire0,
                                                  double hz_ire,
                                                  double output_zero,
                                                  double vsync_ire,
                                                  double out_scale) {
    std::vector<std::uint16_t> out(input.size());
    const double scale = out_scale / hz_ire;
    const double offset = output_zero - vsync_ire * out_scale - ire0 * scale;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double v = static_cast<double>(input[i]) * scale + offset + 0.5;
        out[i] = static_cast<std::uint16_t>(std::max(0.0, std::min(65535.0, v)));
    }
    return out;
}

}  // namespace

DownscaleCoreResult downscale_luma(const DownscaleCoreInput& input) {
    require(input.linelocs.size() >= 2U, "downscale_luma requires linelocs");
    require(input.outlinecount > 0 && input.outlinelen > 0, "downscale_luma requires output geometry");
    require(input.demod.size() >= 4U, "downscale_luma requires demod data");

    DownscaleCoreResult result{};
    const double outscale = input.inlinelen / static_cast<double>(input.outlinelen);
    const int outsamples = input.outlinecount * input.outlinelen;
    const int outline_offset = (input.lineoffset + 1) * input.outlinelen;
    const int total_scaled = outsamples + outline_offset;

    std::vector<double> wowfactors;
    std::vector<double> level_adjusts(static_cast<std::size_t>(total_scaled));
    std::vector<double> interpolated_pixel_locs;
    if (input.wow_spline_degree <= 1) {
        const int line_count = total_scaled / input.outlinelen;
        std::vector<double> line_adjusts(static_cast<std::size_t>(line_count));
        const int max_seg = static_cast<int>(input.linelocs.size()) - 2;
        for (int seg = 0; seg < line_count; ++seg) {
            const int use_seg = std::min(seg, max_seg);
            line_adjusts[static_cast<std::size_t>(seg)] =
                (input.linelocs[static_cast<std::size_t>(use_seg + 1)] - input.linelocs[static_cast<std::size_t>(use_seg)]) /
                input.inlinelen;
        }

        const double median = median_copy(line_adjusts);
        std::vector<double> absdev(line_adjusts.size());
        for (std::size_t i = 0; i < line_adjusts.size(); ++i) absdev[i] = std::abs(line_adjusts[i] - median);
        const double mad = median_copy(absdev);
        const double threshold = (mad > 0.0) ? 15.0 * mad : 0.001;
        for (double& v : line_adjusts) {
            if (std::abs(v - median) > threshold) v = median;
        }

        if (input.keep_debug_arrays) wowfactors.resize(static_cast<std::size_t>(total_scaled));
        if (input.wow_level_adjust_smoothing > 0.0) {
            const double alpha = 1.0 / (input.wow_level_adjust_smoothing * static_cast<double>(input.outlinelen));
            const double one_minus_alpha = 1.0 - alpha;
            level_adjusts[0] = line_adjusts[0];
            for (int seg = 0; seg < line_count; ++seg) {
                const double line_wow = line_adjusts[static_cast<std::size_t>(seg)];
                const int start = seg * input.outlinelen;
                const int end = start + input.outlinelen;
                if (input.keep_debug_arrays) {
                    for (int i = start; i < end; ++i) wowfactors[static_cast<std::size_t>(i)] = line_wow;
                }
                for (int i = (seg == 0 ? 1 : start); i < end; ++i) {
                    level_adjusts[static_cast<std::size_t>(i)] =
                        alpha * line_wow + one_minus_alpha * level_adjusts[static_cast<std::size_t>(i - 1)];
                }
            }
        } else {
            for (int seg = 0; seg < line_count; ++seg) {
                const double line_wow = line_adjusts[static_cast<std::size_t>(seg)];
                const int start = seg * input.outlinelen;
                const int end = start + input.outlinelen;
                for (int i = start; i < end; ++i) {
                    level_adjusts[static_cast<std::size_t>(i)] = line_wow;
                    if (input.keep_debug_arrays) wowfactors[static_cast<std::size_t>(i)] = line_wow;
                }
            }
        }
    } else {
        const std::size_t loc_count = input.linelocs.size();
        std::vector<double> expected_linelocs(loc_count, 0.0);
        for (std::size_t i = 0; i < loc_count; ++i) expected_linelocs[i] = static_cast<double>(i) * input.inlinelen;
        if (input.keep_debug_arrays) wowfactors.resize(static_cast<std::size_t>(total_scaled));
        interpolated_pixel_locs.resize(static_cast<std::size_t>(total_scaled));
        const auto& knots = input.wow_spline_precomputed
            ? input.spline_knots
            : (input.wow_spline_degree == 2 ? build_not_a_knot(expected_linelocs, 2) : build_augknt(expected_linelocs, 3));
        const auto coeffs = input.wow_spline_precomputed
            ? input.spline_coeffs
            : build_spline_coeffs(expected_linelocs, input.linelocs, input.wow_spline_degree);
        require(!knots.empty() && !coeffs.empty(), "missing spline data");
        for (int i = 0; i < total_scaled; ++i) {
            const double x = static_cast<double>(i) * outscale;
            const auto [v, d] = eval_bspline_and_derivative(knots, coeffs, input.wow_spline_degree, x);
            interpolated_pixel_locs[static_cast<std::size_t>(i)] = v;
            level_adjusts[static_cast<std::size_t>(i)] = d;
            if (input.keep_debug_arrays) wowfactors[static_cast<std::size_t>(i)] = d;
        }
        const double median = median_copy(level_adjusts);
        std::vector<double> absdev(level_adjusts.size());
        for (std::size_t i = 0; i < level_adjusts.size(); ++i) absdev[i] = std::abs(level_adjusts[i] - median);
        const double mad = median_copy(absdev);
        const double threshold = (mad > 0.0) ? 15.0 * mad : 0.001;
        for (double& v : level_adjusts) {
            if (std::abs(v - median) > threshold) v = median;
        }

        if (input.wow_level_adjust_smoothing > 0.0) {
            const double alpha = 1.0 / (input.wow_level_adjust_smoothing * static_cast<double>(input.outlinelen));
            const double one_minus_alpha = 1.0 - alpha;
            for (std::size_t i = 1; i < level_adjusts.size(); ++i) {
                level_adjusts[i] = alpha * level_adjusts[i] + one_minus_alpha * level_adjusts[i - 1U];
            }
        }
    }

    result.dsout_float.assign(static_cast<std::size_t>(outsamples), 0.0f);
    const int lineoffset_out_samples = input.outlinelen * (input.lineoffset + 1);
    const int max_valid = static_cast<int>(input.demod.size()) - 3;
    if (input.wow_spline_degree <= 1) {
        const int max_seg = static_cast<int>(input.linelocs.size()) - 2;
        for (int seg = input.lineoffset + 1; seg <= max_seg && seg < input.lineoffset + 1 + input.outlinecount; ++seg) {
            const int start = seg * input.outlinelen;
            const int end = std::min(outsamples + lineoffset_out_samples, start + input.outlinelen);
            const double base = input.linelocs[static_cast<std::size_t>(seg)];
            const double delta =
                input.linelocs[static_cast<std::size_t>(seg + 1)] - input.linelocs[static_cast<std::size_t>(seg)];
            for (int i = start; i < end; ++i) {
                const int within = i - start;
                const double frac = static_cast<double>(within) / static_cast<double>(input.outlinelen);
                const float level_adjust = static_cast<float>(level_adjusts[static_cast<std::size_t>(i)]);
                const float coord = static_cast<float>(base + frac * delta);
                int coord_int = static_cast<int>(coord);
                coord_int = std::max(1, std::min(coord_int, max_valid));
                const float p0 = input.demod[static_cast<std::size_t>(coord_int - 1)];
                const float p1 = input.demod[static_cast<std::size_t>(coord_int)];
                const float p2 = input.demod[static_cast<std::size_t>(coord_int + 1)];
                const float p3 = input.demod[static_cast<std::size_t>(coord_int + 2)];
                const float x = coord - static_cast<float>(coord_int);
                const float a = p2 - p0;
                const float b = 2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3;
                const float c = 3.0f * (p1 - p2) + p3 - p0;
                result.dsout_float[static_cast<std::size_t>(i - lineoffset_out_samples)] =
                    level_adjust * (p1 + 0.5f * x * (a + x * (b + x * c)));
            }
        }
    } else {
        for (int i = lineoffset_out_samples; i < outsamples + lineoffset_out_samples; ++i) {
            const float level_adjust = static_cast<float>(level_adjusts[static_cast<std::size_t>(i)]);
            const float coord = static_cast<float>(interpolated_pixel_locs[static_cast<std::size_t>(i)]);
            int coord_int = static_cast<int>(coord);
            coord_int = std::max(1, std::min(coord_int, max_valid));
            const float p0 = input.demod[static_cast<std::size_t>(coord_int - 1)];
            const float p1 = input.demod[static_cast<std::size_t>(coord_int)];
            const float p2 = input.demod[static_cast<std::size_t>(coord_int + 1)];
            const float p3 = input.demod[static_cast<std::size_t>(coord_int + 2)];
            const float x = coord - static_cast<float>(coord_int);
            const float a = p2 - p0;
            const float b = 2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3;
            const float c = 3.0f * (p1 - p2) + p3 - p0;
            result.dsout_float[static_cast<std::size_t>(i - lineoffset_out_samples)] =
                level_adjust * (p1 + 0.5f * x * (a + x * (b + x * c)));
        }
    }

    if (input.y_comb_limit != 0.0) {
        result.dsout_float = y_comb_cpp(std::move(result.dsout_float), input.outlinelen, input.y_comb_limit);
    }

    if (input.final_output) {
        result.dsout_u16 = hz_to_output_array_cpp(
            result.dsout_float,
            input.ire0,
            input.hz_ire,
            input.output_zero,
            input.vsync_ire,
            input.out_scale);
    }
    if (input.keep_debug_arrays) {
        result.wowfactors = std::move(wowfactors);
        if (!interpolated_pixel_locs.empty()) {
            result.interpolated_pixel_locs = std::move(interpolated_pixel_locs);
        }
    }
    return result;
}

NtscVhsDownscaleResult downscale_ntsc_vhs(const NtscVhsDownscaleInput& input) {
    NtscVhsDownscaleResult result{};
    result.luma = downscale_luma(input.luma);
    if (input.write_chroma) {
        result.chroma = process_chroma(input.chroma);
    }
    return result;
}

}  // namespace vhsdecode_cpp

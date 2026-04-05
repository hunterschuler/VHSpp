#include "vhsdecode_cpp/chroma_phase.h"
#include "vhsdecode_cpp/decode_driver.h"
#include "vhsdecode_cpp/demod_cache.h"
#include "vhsdecode_cpp/downscale_core.h"
#include "vhsdecode_cpp/k1_context.h"
#include "vhsdecode_cpp/metadata_core.h"
#include "vhsdecode_cpp/output_core.h"
#include "vhsdecode_cpp/resync_runtime.h"
#include "vhsdecode_cpp/session_core.h"
#include "vhsdecode_cpp/sync_core.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <limits>
#include <map>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace vhsdecode::cppport;

namespace {

std::unordered_map<std::string, std::string> read_kv(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open kv file: " + path.string());
    std::unordered_map<std::string, std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        out.emplace(line.substr(0, pos), line.substr(pos + 1));
    }
    return out;
}

template <typename T>
T parse_num(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    const auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) throw std::runtime_error("missing numeric key: " + key);
    std::istringstream iss(it->second);
    T value{};
    iss >> value;
    return value;
}

template <typename T>
std::optional<T> parse_opt_num(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    const auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return std::nullopt;
    std::istringstream iss(it->second);
    T value{};
    iss >> value;
    return value;
}

bool parse_bool(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    const auto it = kv.find(key);
    if (it == kv.end()) return false;
    return it->second == "1" || it->second == "true" || it->second == "True";
}

std::string parse_str(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    const auto it = kv.find(key);
    return (it == kv.end()) ? std::string{} : it->second;
}

template <typename T>
std::vector<T> read_bin(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("failed to open binary file: " + path.string());
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if ((size % static_cast<std::streamsize>(sizeof(T))) != 0) {
        throw std::runtime_error("bad binary size for: " + path.string());
    }
    std::vector<T> out(static_cast<std::size_t>(size / sizeof(T)));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return out;
}

double median_of(std::vector<double> values) {
    if (values.empty()) return 0.0;
    auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2U);
    std::nth_element(values.begin(), mid, values.end());
    if ((values.size() % 2U) == 1U) return *mid;
    const double hi = *mid;
    std::nth_element(values.begin(), mid - 1, values.end());
    return (hi + *(mid - 1)) * 0.5;
}

double percentile_of(std::vector<double> values, double pct) {
    if (values.empty()) return 0.0;
    if (pct <= 0.0) {
        return *std::min_element(values.begin(), values.end());
    }
    if (pct >= 100.0) {
        return *std::max_element(values.begin(), values.end());
    }
    const double pos = (pct / 100.0) * static_cast<double>(values.size() - 1U);
    const auto lo_idx = static_cast<std::size_t>(std::floor(pos));
    const auto hi_idx = static_cast<std::size_t>(std::ceil(pos));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(lo_idx), values.end());
    const double lo = values[lo_idx];
    if (lo_idx == hi_idx) return lo;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(hi_idx), values.end());
    const double hi = values[hi_idx];
    const double frac = pos - static_cast<double>(lo_idx);
    return lo + (hi - lo) * frac;
}

std::optional<IirFilter> read_ba_optional(const fs::path& path) {
    if (!fs::exists(path)) return std::nullopt;
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open ba file: " + path.string());
    IirFilter filt;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("b=", 0) == 0) {
            std::istringstream iss(line.substr(2));
            std::string tok;
            while (std::getline(iss, tok, ',')) filt.b.push_back(std::stod(tok));
        } else if (line.rfind("a=", 0) == 0) {
            std::istringstream iss(line.substr(2));
            std::string tok;
            while (std::getline(iss, tok, ',')) filt.a.push_back(std::stod(tok));
        }
    }
    return filt;
}

SosFilter read_sos_flat(const fs::path& path) {
    const auto flat = read_bin<double>(path);
    if ((flat.size() % 6U) != 0U) throw std::runtime_error("bad sos size");
    SosFilter sos;
    for (std::size_t i = 0; i < flat.size(); i += 6) {
        sos.sections.push_back({flat[i + 0], flat[i + 1], flat[i + 2], flat[i + 3], flat[i + 4], flat[i + 5]});
    }
    return sos;
}

std::vector<std::vector<double>> read_phase_matrix(const fs::path& path) {
    const auto flat = read_bin<double>(path);
    if ((flat.size() % 4U) != 0U) throw std::runtime_error("heterodyne flat size must be divisible by 4");
    const std::size_t len = flat.size() / 4U;
    std::vector<std::vector<double>> out(4, std::vector<double>(len));
    for (std::size_t p = 0; p < 4; ++p) {
        std::copy(flat.begin() + static_cast<std::ptrdiff_t>(p * len),
                  flat.begin() + static_cast<std::ptrdiff_t>((p + 1) * len),
                  out[p].begin());
    }
    return out;
}

void write_u16(std::ofstream& out, const std::vector<std::uint16_t>& data) {
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(std::uint16_t)));
}

K1BuildConfig load_k1_config(const std::unordered_map<std::string, std::string>& kv) {
    K1BuildConfig cfg{};
    cfg.blocklen = parse_num<std::size_t>(kv, "blocklen");
    cfg.freq_hz = parse_num<double>(kv, "freq_hz");
    cfg.fsc_mhz = parse_num<double>(kv, "fsc_mhz");
    cfg.hz_ire = parse_num<double>(kv, "hz_ire");
    cfg.vsync_ire = parse_num<double>(kv, "vsync_ire");
    cfg.fps = parse_num<double>(kv, "fps");
    cfg.frame_lines = parse_num<int>(kv, "frame_lines");
    cfg.max_field_lines = parse_num<int>(kv, "max_field_lines");
    cfg.outlinelen = parse_num<int>(kv, "outlinelen");
    cfg.tape_format = "VHS";
    if (auto tape = parse_str(kv, "tape_format"); !tape.empty()) {
        static std::string owned_tape;
        owned_tape = tape;
        cfg.tape_format = owned_tape.c_str();
    }

    auto& dp = cfg.decoder_params;
    dp.video_bpf_supergauss = parse_bool(kv, "video_bpf_supergauss");
    dp.video_bpf_low = parse_num<double>(kv, "video_bpf_low");
    dp.video_bpf_high = parse_num<double>(kv, "video_bpf_high");
    dp.video_bpf_order = parse_num<int>(kv, "video_bpf_order");
    dp.video_lpf_extra_order = parse_num<int>(kv, "video_lpf_extra_order");
    dp.video_lpf_extra = parse_num<double>(kv, "video_lpf_extra");
    dp.video_hpf_extra_order = parse_num<int>(kv, "video_hpf_extra_order");
    dp.video_hpf_extra = parse_num<double>(kv, "video_hpf_extra");
    dp.video_rf_peak_freq = parse_opt_num<double>(kv, "video_rf_peak_freq");
    dp.video_rf_peak_gain = parse_num<double>(kv, "video_rf_peak_gain");
    dp.video_rf_peak_bandwidth = parse_num<double>(kv, "video_rf_peak_bandwidth");
    dp.fm_audio_channel_0_freq = parse_opt_num<double>(kv, "fm_audio_channel_0_freq");
    dp.fm_audio_channel_1_freq = parse_opt_num<double>(kv, "fm_audio_channel_1_freq");
    dp.start_rf_linear = parse_opt_num<double>(kv, "start_rf_linear");
    dp.boost_rf_linear_0 = parse_opt_num<double>(kv, "boost_rf_linear_0");
    dp.boost_rf_linear_20 = parse_num<double>(kv, "boost_rf_linear_20");
    dp.boost_rf_linear_double = parse_bool(kv, "boost_rf_linear_double");
    dp.boost_bpf_low = parse_num<double>(kv, "boost_bpf_low");
    dp.boost_bpf_high = parse_num<double>(kv, "boost_bpf_high");
    dp.boost_bpf_mult = parse_opt_num<double>(kv, "boost_bpf_mult").value_or(0.0);
    dp.deemph_gain = parse_num<double>(kv, "deemph_gain");
    dp.deemph_mid = parse_num<double>(kv, "deemph_mid");
    dp.deemph_q = parse_num<double>(kv, "deemph_q");
    dp.video_lpf_supergauss = parse_bool(kv, "video_lpf_supergauss");
    dp.video_lpf_freq = parse_num<double>(kv, "video_lpf_freq");
    dp.video_lpf_order = parse_num<int>(kv, "video_lpf_order");
    dp.nonlinear_amp_lpf_freq = parse_num<double>(kv, "nonlinear_amp_lpf_freq");
    dp.nonlinear_bandpass_upper = parse_opt_num<double>(kv, "nonlinear_bandpass_upper");
    dp.nonlinear_bandpass_order = parse_num<int>(kv, "nonlinear_bandpass_order");
    dp.nonlinear_highpass_freq = parse_num<double>(kv, "nonlinear_highpass_freq");
    dp.nonlinear_highpass_limit_l = parse_num<double>(kv, "nonlinear_highpass_limit_l");
    dp.nonlinear_highpass_limit_h = parse_num<double>(kv, "nonlinear_highpass_limit_h");
    dp.use_sub_deemphasis = parse_bool(kv, "use_sub_deemphasis");
    dp.nonlinear_exp_scaling = parse_num<double>(kv, "nonlinear_exp_scaling");
    dp.nonlinear_scaling_1 = parse_opt_num<double>(kv, "nonlinear_scaling_1");
    dp.nonlinear_scaling_2 = parse_opt_num<double>(kv, "nonlinear_scaling_2");
    dp.nonlinear_logistic_mid = parse_opt_num<double>(kv, "nonlinear_logistic_mid");
    dp.nonlinear_logistic_rate = parse_opt_num<double>(kv, "nonlinear_logistic_rate");
    dp.nonlinear_static_factor = parse_opt_num<double>(kv, "nonlinear_static_factor");
    dp.nonlinear_deviation = parse_opt_num<double>(kv, "nonlinear_deviation");
    dp.color_under_carrier = parse_num<double>(kv, "color_under_carrier");
    dp.chroma_bpf_upper = parse_num<double>(kv, "chroma_bpf_upper");
    dp.chroma_bpf_order = parse_num<int>(kv, "chroma_bpf_order");
    dp.chroma_bpf_lower = parse_num<double>(kv, "chroma_bpf_lower");
    dp.chroma_audio_notch_freq = parse_opt_num<double>(kv, "chroma_audio_notch_freq");
    if (parse_bool(kv, "video_eq_present")) {
        VideoEqDesignParams veq{};
        veq.corner = parse_num<double>(kv, "video_eq_corner");
        veq.transition = parse_num<double>(kv, "video_eq_transition");
        veq.order_limit = parse_num<int>(kv, "video_eq_order_limit");
        dp.video_eq = veq;
    }

    auto& rt = cfg.runtime;
    rt.high_boost = parse_opt_num<double>(kv, "high_boost");
    rt.disable_diff_demod = parse_bool(kv, "disable_diff_demod");
    rt.diff_demod_check_value = parse_num<double>(kv, "diff_demod_check_value");
    rt.enable_video_notch = parse_bool(kv, "enable_video_notch");
    rt.video_notch_freq = parse_opt_num<double>(kv, "video_notch_freq").value_or(0.0);
    rt.video_notch_q = parse_num<double>(kv, "video_notch_q");
    rt.enable_chroma_audio_notch = parse_bool(kv, "enable_chroma_audio_notch");
    rt.enable_chroma_trap = parse_bool(kv, "enable_chroma_trap");
    rt.enable_nldeemp = parse_bool(kv, "enable_nldeemp");
    rt.enable_subdeemp = parse_bool(kv, "enable_subdeemp");
    rt.enable_fsc_notch = parse_bool(kv, "enable_fsc_notch");
    rt.color_under = parse_bool(kv, "color_under");
    rt.chroma_offset = parse_num<int>(kv, "chroma_offset");
    rt.sharpness_level = parse_num<double>(kv, "sharpness_level");
    rt.fm_audio_notch_q = parse_num<int>(kv, "fm_audio_notch_q");
    rt.do_cafc = parse_bool(kv, "do_cafc");
    rt.cafc_output_freq_half_hz = parse_opt_num<double>(kv, "cafc_output_freq_half_hz");
    return cfg;
}

struct PhaseConfig {
    vhsdecode_cpp::ChromaPhaseInput input{};
};

PhaseConfig load_phase_config(const fs::path& dir) {
    const auto kv = read_kv(dir / "phase_config.kv");
    PhaseConfig out{};
    out.input.chroma_filter = read_sos_flat(dir / "fchroma_final_sos.f64");
    out.input.chroma_heterodyne = read_phase_matrix(dir / "chroma_heterodyne_flat.f64");
    out.input.lineoffset = parse_num<int>(kv, "lineoffset");
    out.input.linesout = parse_num<int>(kv, "linesout");
    out.input.outwidth = parse_num<int>(kv, "outwidth");
    out.input.burst_start = parse_num<int>(kv, "burst_start");
    out.input.burst_end = parse_num<int>(kv, "burst_end");
    out.input.detect_chroma_track_phase = parse_num<int>(kv, "detect_chroma_track_phase") != 0;
    out.input.rotation_check_start_line = parse_num<int>(kv, "rotation_check_start_line");
    out.input.is_ntsc = parse_num<int>(kv, "is_ntsc") != 0;
    if (parse_num<int>(kv, "track_phase_present") != 0) {
        out.input.chroma_rotation_index = parse_num<int>(kv, "track_phase");
    }
    out.input.burst_sin = read_bin<double>(dir / "fsc_wave.f64");
    out.input.burst_cos = read_bin<double>(dir / "fsc_cos_wave.f64");
    const fs::path rotation_path = dir / "chroma_rotation.txt";
    if (fs::exists(rotation_path)) {
        std::ifstream in(rotation_path);
        std::string line;
        std::vector<int> rot;
        while (std::getline(in, line)) {
            if (!line.empty()) rot.push_back(std::stoi(line));
        }
        if (!rot.empty()) out.input.chroma_rotation = rot;
    }
    return out;
}

struct FieldConfig {
    vhsdecode_cpp::NtscVhsDownscaleInput input{};
};

FieldConfig load_field_config(const fs::path& dir) {
    const auto dkv = read_kv(dir / "downscale_config.kv");
    const auto pkv = read_kv(dir / "process_config.kv");
    FieldConfig out{};
    out.input.write_chroma = true;
    out.input.luma.lineoffset = parse_num<int>(dkv, "lineoffset");
    out.input.luma.outlinecount = parse_num<int>(dkv, "outlinecount");
    out.input.luma.outlinelen = parse_num<int>(dkv, "outlinelen");
    out.input.luma.inlinelen = parse_num<double>(dkv, "inlinelen");
    out.input.luma.wow_level_adjust_smoothing = parse_num<double>(dkv, "wow_level_adjust_smoothing");
    out.input.luma.y_comb_limit = parse_num<double>(dkv, "y_comb_limit");
    out.input.luma.final_output = parse_num<int>(dkv, "final_output") != 0;
    out.input.luma.export_raw_tbc = parse_num<int>(dkv, "export_raw_tbc") != 0;
    out.input.luma.ire0 = parse_num<double>(dkv, "ire0");
    out.input.luma.hz_ire = parse_num<double>(dkv, "hz_ire");
    out.input.luma.output_zero = parse_num<double>(dkv, "output_zero");
    out.input.luma.vsync_ire = parse_num<double>(dkv, "vsync_ire");
    out.input.luma.out_scale = parse_num<double>(dkv, "out_scale");

    out.input.chroma.chroma_heterodyne = read_phase_matrix(dir / "chroma_heterodyne_flat.f64");
    out.input.chroma.fchroma_final = read_sos_flat(dir / "fchroma_final_sos.f64");
    if (fs::exists(dir / "cafc_bandpass_sos.f64")) out.input.chroma.cafc_chroma_bandpass = read_sos_flat(dir / "cafc_bandpass_sos.f64");
    out.input.chroma.fvideo_notch = read_ba_optional(dir / "fvideo_notch.ba");
    out.input.chroma.chroma_audio_notch = read_ba_optional(dir / "chroma_audio_notch.ba");
    out.input.chroma.chroma_deemphasis = read_ba_optional(dir / "chroma_deemphasis.ba");
    out.input.chroma.lineoffset = parse_num<int>(pkv, "lineoffset");
    out.input.chroma.linesout = parse_num<int>(pkv, "linesout");
    out.input.chroma.outwidth = parse_num<int>(pkv, "outwidth");
    out.input.chroma.burst_start = parse_num<int>(pkv, "burst_start");
    out.input.chroma.burst_end = parse_num<int>(pkv, "burst_end");
    out.input.chroma.cafc_move = parse_num<int>(pkv, "cafc_move");
    out.input.chroma.is_ntsc = parse_num<int>(pkv, "is_ntsc") != 0;
    out.input.chroma.do_cafc = parse_num<int>(pkv, "do_cafc") != 0;
    out.input.chroma.disable_deemph = parse_num<int>(pkv, "disable_deemph") != 0;
    out.input.chroma.disable_comb = parse_num<int>(pkv, "disable_comb") != 0;
    out.input.chroma.disable_tracking_cafc = parse_num<int>(pkv, "disable_tracking_cafc") != 0;
    out.input.chroma.disable_phase_correction = parse_num<int>(pkv, "disable_phase_correction") != 0;
    out.input.chroma.do_chroma_deemphasis = parse_num<int>(pkv, "do_chroma_deemphasis") != 0;
    out.input.chroma.enable_video_notch = parse_num<int>(pkv, "enable_video_notch") != 0;
    if (parse_num<int>(pkv, "burst_phase_avg_present") != 0) {
        out.input.chroma.burst_phase_avg = parse_num<double>(pkv, "burst_phase_avg");
    }
    out.input.chroma.burst_abs_ref = parse_num<double>(pkv, "burst_abs_ref");
    if (out.input.chroma.do_cafc) {
        ChromaAfcConfig cfg{};
        cfg.demod_rate_hz = parse_num<double>(pkv, "cafc_demod_rate_hz");
        cfg.under_ratio = parse_num<double>(pkv, "cafc_under_ratio");
        cfg.fps = parse_num<double>(pkv, "cafc_fps");
        cfg.frame_lines = parse_num<int>(pkv, "cafc_frame_lines");
        cfg.max_field_lines = parse_num<int>(pkv, "cafc_max_field_lines");
        cfg.outlinelen = parse_num<int>(pkv, "cafc_outlinelen");
        cfg.fsc_mhz = parse_num<double>(pkv, "cafc_fsc_mhz");
        cfg.color_under_carrier_hz = parse_num<double>(pkv, "cafc_color_under_carrier_hz");
        cfg.chroma_bandpass_order = parse_num<int>(pkv, "cafc_chroma_bandpass_order");
        cfg.linearize = parse_num<int>(pkv, "cafc_linearize") != 0;
        cfg.do_cafc = true;
        cfg.chroma_bpf_lower_hz = parse_num<double>(pkv, "cafc_chroma_bpf_lower_hz");
        static std::optional<ChromaAfc> cafc;
        cafc.emplace(cfg);
        cafc->setCC(parse_num<double>(pkv, "cafc_cc_hz"));
        cafc->setCCPhase(parse_num<double>(pkv, "cafc_cc_phase_rad"));
        out.input.chroma.chroma_afc = &*cafc;
    }
    return out;
}

struct RuntimeState {
    int prev_first_hsync_readloc = -1;
    double prev_first_hsync_loc = -1.0;
    double prev_hsync_diff = -1.0;
    int prev_first_field = -1;
    std::optional<int> track_phase;
    int seq_no = 0;
};

struct DriverPerfStats {
    double read_s = 0.0;
    double k2_s = 0.0;
    double k3_s = 0.0;
    double k4_s = 0.0;
    double write_s = 0.0;
};

double ire_to_hz_local(double ire0, double hz_ire, double ire) {
    return ire0 + hz_ire * ire;
}

double compute_meanlinelen(const std::vector<vhsdecode_cpp::ValidPulse>& validpulses, double inlinelen) {
    int best_start = -1;
    int best_len = -1;
    int cur_start = -1;
    int cur_len = 0;
    for (std::size_t i = 0; i < validpulses.size(); ++i) {
        if (validpulses[i].type != 0) {
            if (cur_start != -1 && cur_len > best_len) {
                best_start = cur_start;
                best_len = cur_len;
            }
            cur_start = -1;
            cur_len = 0;
        } else if (cur_start == -1) {
            cur_start = static_cast<int>(i);
            cur_len = 0;
        } else {
            cur_len += 1;
        }
    }
    if (cur_start != -1 && cur_len > best_len) {
        best_start = cur_start;
        best_len = cur_len;
    }
    std::vector<double> lens;
    for (int i = best_start + 1; i < best_start + best_len; ++i) {
        const double len = static_cast<double>(validpulses[static_cast<std::size_t>(i)].start - validpulses[static_cast<std::size_t>(i - 1)].start);
        const double ratio = len / inlinelen;
        if (ratio >= 0.95 && ratio <= 1.05) lens.push_back(len);
    }
    if (lens.empty()) return inlinelen;
    double sum = 0.0;
    for (double v : lens) sum += v;
    return sum / static_cast<double>(lens.size());
}

void sync_to_burst(std::vector<double>& linelocs,
                   std::vector<std::uint8_t>& linebad,
                   int outlinelen,
                   double burst_avg_phase,
                   const std::vector<vhsdecode_cpp::PhaseSequenceEntry>& phase_sequence) {
    if (linelocs.size() < 2U) return;

    std::map<int, double> adjs;
    for (std::size_t idx = 9; idx < phase_sequence.size(); ++idx) {
        const auto& row = phase_sequence[idx];
        const int line_number = row.line_number;
        if (line_number < 0 || static_cast<std::size_t>(line_number + 1) >= linelocs.size()) continue;
        const double phase_delta = std::fmod(burst_avg_phase - row.burst_phase + 180.0, 360.0) - 180.0;
        const double line_start = linelocs[static_cast<std::size_t>(line_number)];
        const double line_end = linelocs[static_cast<std::size_t>(line_number + 1)];
        const double line_length = line_end - line_start;
        const double scale = line_length / static_cast<double>(outlinelen);
        adjs[line_number] = (phase_delta / 360.0) * 4.0 * scale;
    }

    const std::size_t max_lines = std::min<std::size_t>(266U, linelocs.size());
    for (std::size_t l = 1; l < max_lines; ++l) {
        if (adjs.find(static_cast<int>(l)) == adjs.end() && l < linebad.size()) {
            linebad[l] = 1;
        }
    }
    if (adjs.empty()) return;

    std::vector<double> adj_values;
    adj_values.reserve(adjs.size());
    for (const auto& [_, v] : adjs) adj_values.push_back(v);
    const double adjs_median = median_of(adj_values);
    double lastvalid_adj = adjs_median;

    for (std::size_t l = 0; l < max_lines; ++l) {
        const auto it = adjs.find(static_cast<int>(l));
        if (it != adjs.end() && std::abs(it->second - adjs_median) <= 2.0) {
            linelocs[l] += it->second;
            lastvalid_adj = it->second;
        } else {
            linelocs[l] += lastvalid_adj;
        }
    }
}

void apply_ntsc_phase_offset(std::vector<double>& linelocs, double sample_rate_mhz) {
    constexpr double kShift33Rad = 83.0 * (3.14159265358979323846 / 180.0);
    const double sample_shift = (-kShift33Rad) * (sample_rate_mhz / (4.0 * 315.0 / 88.0));
    for (double& loc : linelocs) loc += sample_shift;
}

void fix_badlines(std::vector<double>& linelocs,
                  const std::vector<double>& linelocs_backup,
                  const std::vector<std::uint8_t>& linebad,
                  int lineoffset) {
    if (linelocs.empty() || linebad.empty()) return;
    const std::size_t n = std::min(linelocs.size(), linebad.size());
    const int last_from_bottom = static_cast<int>(n) - 16;
    const int firstcheck = 1;

    for (std::size_t idx = 0; idx < n; ++idx) {
        if (!linebad[idx]) continue;
        const int l = static_cast<int>(idx);
        int prevgood = l - 1;
        int nextgood = l + 1;

        while (prevgood >= 0 && linebad[static_cast<std::size_t>(prevgood)]) {
            --prevgood;
        }
        while (nextgood < static_cast<int>(n) && linebad[static_cast<std::size_t>(nextgood)]) {
            ++nextgood;
        }

        if (prevgood >= firstcheck && nextgood < (static_cast<int>(n) + lineoffset)) {
            if (nextgood > last_from_bottom) {
                if (prevgood > last_from_bottom + 4) {
                    const double guess_len = linelocs[static_cast<std::size_t>(prevgood)] -
                                             linelocs[static_cast<std::size_t>(prevgood - 1)];
                    linelocs[idx] = linelocs[static_cast<std::size_t>(l - 1)] + guess_len;
                }
            } else {
                const double gap = (linelocs[static_cast<std::size_t>(nextgood)] -
                                    linelocs[static_cast<std::size_t>(prevgood)]) /
                                   static_cast<double>(nextgood - prevgood);
                linelocs[idx] = (gap * static_cast<double>(l - prevgood)) +
                                linelocs[static_cast<std::size_t>(prevgood)];
            }
        } else if (idx < linelocs_backup.size()) {
            linelocs[idx] = linelocs_backup[idx];
        }
    }
}

vhsdecode_cpp::PulseTimingRanges compute_timings(const std::vector<vhsdecode_cpp::Pulse>& pulses,
                                                 double sample_rate_mhz) {
    constexpr double hsyncPulseUS = 4.7;
    constexpr double eqPulseUS = 2.35;
    constexpr double vsyncPulseUS = 27.1;
    std::vector<int> hlens;
    const double hsync_typical = sample_rate_mhz * hsyncPulseUS;
    const double hsync_checkmin = sample_rate_mhz * (hsyncPulseUS - 1.75);
    const double hsync_checkmax = sample_rate_mhz * (hsyncPulseUS + 2.0);
    for (const auto& p : pulses) {
        if (p.len >= hsync_checkmin && p.len <= hsync_checkmax) hlens.push_back(p.len);
    }
    double hsync_median = hsync_typical;
    if (!hlens.empty()) {
        std::sort(hlens.begin(), hlens.end());
        hsync_median = static_cast<double>(hlens[hlens.size() / 2]);
    }
    const double hsync_min = hsync_median + (sample_rate_mhz * -0.7);
    const double hsync_max = hsync_median + (sample_rate_mhz * 0.7);
    const double hsync_offset = hsync_median - hsync_typical;
    const double eq_tol = 0.9;
    const double eq_min = (sample_rate_mhz * (eqPulseUS - eq_tol)) + hsync_offset;
    const double eq_max = (sample_rate_mhz * (eqPulseUS + eq_tol)) + hsync_offset;
    const double vsync_min = (sample_rate_mhz * (vsyncPulseUS * 0.5)) + hsync_offset;
    const double vsync_max = (sample_rate_mhz * (vsyncPulseUS + 1.0)) + hsync_offset;
    return {hsync_min, hsync_max, eq_min, eq_max, vsync_min, vsync_max};
}

struct DetectedLevels {
    double ire0 = 0.0;
    double hz_ire = 0.0;
    double vsync_ire = -40.0;
    double out_scale = 0.0;
    bool valid = false;
};

struct DecodedField {
    bool valid = false;
    bool is_first_field = false;
    int readloc = 0;
    int next_offset = 0;
    int sync_conf = 100;
    int raw_pulses = 0;
    int valid_pulses = 0;
    bool has_first_hsync = false;
    std::string invalid_reason;
    double resync_last_pulse_threshold = 0.0;
    std::optional<double> resync_field_sync;
    std::optional<double> resync_field_blank;
    std::optional<double> resync_serration_sync;
    std::optional<double> resync_serration_blank;
    double resync_sync_level_bias = 0.0;
    std::vector<double> linelocs;
    std::vector<std::uint8_t> linebad;
    DetectedLevels detected_levels;
    double output_ire0 = 0.0;
    double output_hz_ire = 0.0;
    double output_vsync_ire = -40.0;
    double output_out_scale = 0.0;
    std::vector<float> luma_float;
    std::vector<std::uint16_t> luma_u16;
    std::vector<std::uint16_t> chroma_u16;
};

DetectedLevels detect_levels_ntsc(const std::vector<double>& demod,
                                  const std::vector<double>& demod_05,
                                  const std::vector<double>& linelocs_for_levels,
                                  double freq_mhz,
                                  double current_ire0,
                                  double current_hz_ire,
                                  double current_vsync_ire,
                                  int outlinecount) {
    constexpr double kLinePeriodUs = 1.0 / ((315.0 / 88.0) / 227.5);
    static const std::vector<std::array<double, 3>> kWhiteLocs = {
        {20, 14, 12},
        {20, 52, 8},
        {13, 13, 15},
        {11, 12, 45},
    };
    static const std::vector<std::array<double, 4>> kCodeLocs = {
        {16, 12, 48, 85},
        {17, 12, 48, 85},
        {10, 13, 39, 85},
    };

    const double rf_linelen = freq_mhz * kLinePeriodUs;
    const double samplesperline = freq_mhz / rf_linelen;

    auto get_linelen = [&](int line) -> double {
        if (linelocs_for_levels.empty()) return rf_linelen;
        if (line >= outlinecount && static_cast<std::size_t>(line) < linelocs_for_levels.size()) {
            return linelocs_for_levels[static_cast<std::size_t>(line)] -
                   linelocs_for_levels[static_cast<std::size_t>(line - 1)];
        }
        if (line > 0 && static_cast<std::size_t>(line + 1) < linelocs_for_levels.size()) {
            return (linelocs_for_levels[static_cast<std::size_t>(line + 1)] -
                    linelocs_for_levels[static_cast<std::size_t>(line - 1)]) * 0.5;
        }
        if (line == 0 && linelocs_for_levels.size() > 1U) {
            return linelocs_for_levels[1] - linelocs_for_levels[0];
        }
        return rf_linelen;
    };
    auto usectoinpx = [&](double us, int line) -> double {
        return us * (samplesperline * get_linelen(line));
    };
    auto make_slice = [&](int line, double begin_us, double length_us) -> std::pair<std::size_t, std::size_t> {
        if (static_cast<std::size_t>(line) >= linelocs_for_levels.size()) return {0U, 0U};
        const double start = linelocs_for_levels[static_cast<std::size_t>(line)] + usectoinpx(begin_us, line);
        const double end = start + usectoinpx(length_us, line) + 1.0;
        const auto s = static_cast<std::size_t>(std::max(0.0, std::floor(start)));
        const auto e = static_cast<std::size_t>(std::max(0.0, std::ceil(end)));
        return {s, e};
    };
    auto slice_vec = [](const std::vector<double>& src, std::size_t s, std::size_t e) -> std::vector<double> {
        if (s >= src.size()) return {};
        e = std::min(e, src.size());
        if (e <= s) return {};
        return std::vector<double>(src.begin() + static_cast<std::ptrdiff_t>(s),
                                   src.begin() + static_cast<std::ptrdiff_t>(e));
    };
    auto hztoire_local = [&](double hz) -> double {
        return (hz - current_ire0) / current_hz_ire;
    };

    std::vector<double> ire100_hzs;
    for (const auto& wl : kWhiteLocs) {
        const auto [s, e] = make_slice(static_cast<int>(wl[0]), wl[1], wl[2]);
        auto cut = slice_vec(demod, s, e);
        if (cut.empty()) continue;
        const double freq = percentile_of(std::move(cut), 50.0);
        const double freq_ire = hztoire_local(freq);
        if (freq_ire >= 95.0 && freq_ire <= 110.0) ire100_hzs.push_back(freq);
    }
    for (const auto& wl : kCodeLocs) {
        const auto [s, e] = make_slice(static_cast<int>(wl[0]), wl[1], wl[2]);
        auto cut = slice_vec(demod, s, e);
        if (cut.empty()) continue;
        const double freq = percentile_of(std::move(cut), wl[3]);
        const double freq_ire = hztoire_local(freq);
        if (freq_ire >= 95.0 && freq_ire <= 110.0) ire100_hzs.push_back(freq);
    }

    std::vector<double> sync_hzs;
    std::vector<double> ire0_hzs;
    for (int l = 12; l < outlinecount; ++l) {
        const auto [s_sync, e_sync] = make_slice(l, 0.25, 4.0);
        const auto [s_ire0, e_ire0] = make_slice(l, 8.05, 1.15);
        if (e_sync > demod_05.size() || e_ire0 > demod_05.size()) continue;
        const double thislinelen =
            linelocs_for_levels[static_cast<std::size_t>(l)] -
            linelocs_for_levels[static_cast<std::size_t>(l - 1)];
        const double adj = rf_linelen / thislinelen;
        if (adj < 0.98 || adj > 1.02) continue;
        auto sync_cut = slice_vec(demod_05, s_sync, e_sync);
        auto ire0_cut = slice_vec(demod_05, s_ire0, e_ire0);
        if (sync_cut.empty() || ire0_cut.empty()) continue;
        sync_hzs.push_back(median_of(std::move(sync_cut)) / adj);
        ire0_hzs.push_back(median_of(std::move(ire0_cut)) / adj);
    }

    const double vsync_hz = current_ire0 + current_hz_ire * current_vsync_ire;
    const double m_synchz = sync_hzs.empty() ? vsync_hz : median_of(sync_hzs);
    const double m_ire0hz = ire0_hzs.empty() ? current_ire0 : median_of(ire0_hzs);
    const double m_ire100hz = ire100_hzs.empty() ? (current_ire0 + current_hz_ire * 100.0) : median_of(ire100_hzs);
    const double hz_ire = (m_ire100hz - m_ire0hz) / 100.0;
    if (!std::isfinite(hz_ire) || std::abs(hz_ire) < 1.0) return {};
    const double vsync_ire = (m_synchz - m_ire0hz) / hz_ire;
    if (!std::isfinite(vsync_ire)) return {};

    DetectedLevels out;
    out.ire0 = m_ire0hz;
    out.hz_ire = hz_ire;
    out.vsync_ire = vsync_ire;
    out.out_scale = (0xC800 - 0x0400) / (100.0 - vsync_ire);
    out.valid = true;
    return out;
}

int compute_sync_confidence(const std::vector<double>& linelocs,
                            int lineoffset,
                            int linecount,
                            int base_confidence) {
    if (linelocs.size() < 3U) return std::max(0, base_confidence);
    const int start = std::max(0, lineoffset);
    const int end = std::min(static_cast<int>(linelocs.size()), lineoffset + linecount);
    if ((end - start) < 3) return std::max(0, base_confidence);
    double lld2max = 0.0;
    for (int i = start; i + 2 < end; ++i) {
        const double d0 = linelocs[static_cast<std::size_t>(i + 1)] - linelocs[static_cast<std::size_t>(i)];
        const double d1 = linelocs[static_cast<std::size_t>(i + 2)] - linelocs[static_cast<std::size_t>(i + 1)];
        lld2max = std::max(lld2max, d1 - d0);
    }
    int newconf = 100;
    if (lld2max > 4.0) {
        newconf = static_cast<int>(50 - (5.0 * (lld2max > 4.0 ? 1.0 : 0.0)));
    }
    newconf = std::max(newconf, 0);
    return std::min(base_confidence, newconf);
}

template <typename T>
void write_bin_vec(const fs::path& path, const std::vector<T>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) throw std::runtime_error("failed to open debug dump: " + path.string());
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(T)));
}

template <typename T>
void write_bin_vec_once(const fs::path& path, const std::vector<T>& data) {
    if (fs::exists(path)) return;
    write_bin_vec(path, data);
}

template <typename T>
void maybe_write_bin_vec(const fs::path& dir, const char* name, const std::vector<T>& data) {
    if (dir.empty()) return;
    fs::create_directories(dir);
    const fs::path out = dir / name;
    if (fs::exists(out)) return;
    write_bin_vec(out, data);
}

void maybe_write_text(const fs::path& dir, const char* name, const std::string& text) {
    if (dir.empty()) return;
    fs::create_directories(dir);
    const fs::path out = dir / name;
    if (fs::exists(out)) return;
    std::ofstream f(out);
    if (!f.is_open()) throw std::runtime_error("failed to open debug text dump: " + out.string());
    f << text;
}

std::optional<DecodedField> decode_field(std::uint64_t start,
                                         vhsdecode_cpp::DemodCache& reader,
                                         std::size_t blockcut,
                                         const K1BuildConfig& k1_cfg,
                                         vhsdecode_cpp::ResyncRuntime& resync,
                                         const PhaseConfig& phase_cfg,
                                         const FieldConfig& field_cfg,
                                         RuntimeState& runtime,
                                         DriverPerfStats& perf,
                                         const fs::path& debug_dump_dir = {},
                                         bool debug_dump_this_field = false) {
    const std::uint64_t readloc = (start > static_cast<std::uint64_t>(blockcut))
        ? (start - static_cast<std::uint64_t>(blockcut))
        : 0U;
    // LOUD FAITHFUL-PORT NOTE:
    // Upstream decodefield() uses self.readlen, and self.readlen for NTSC is
    // derived from rf.linelen (input-domain samples per line), NOT the later
    // field/downscale inlinelen. Using the K4/output inlinelen here blows the
    // live window up from 622592 to 952320 samples on native TAPE_1 and
    // poisons K2 even though K1/get_pulses are otherwise fine.
    const double rf_linelen =
        k1_cfg.freq_hz / (k1_cfg.fps * static_cast<double>(k1_cfg.frame_lines));
    const int rf_linelen_i = static_cast<int>(std::lround(rf_linelen));
    const std::uint64_t readlen =
        ((static_cast<std::uint64_t>(rf_linelen_i) * 350ULL) / 16384ULL) * 16384ULL;
    // LOUD FAITHFUL-PORT NOTE:
    // The live decoder's field scheduling and metadata fidelity currently
    // match upstream best when decodefield() aligns reads on the demod-cache
    // cut stride, not raw blocklen. A raw-block alignment experiment did
    // assemble a larger local window on the damaged 40 MHz stretch, but it
    // also shifted fileLoc by one cut block and regressed field 35 badly.
    // Keep the session-faithful cut-stride alignment here while the remaining
    // damaged-field mismatch is chased further downstream.
    const std::uint64_t decode_blocksize = static_cast<std::uint64_t>(reader.blocksize());
    const std::uint64_t readloc_block = readloc / decode_blocksize;
    const std::uint64_t numblocks = (readlen / decode_blocksize) + 2ULL;
    const std::uint64_t cache_begin = readloc_block * decode_blocksize;
    const std::uint64_t length = numblocks * decode_blocksize;
    const auto t_read_0 = std::chrono::steady_clock::now();
    auto window = reader.read(cache_begin, length, false);
    perf.read_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_read_0).count();
    if (!window.has_value()) return std::nullopt;
    if (debug_dump_this_field && !debug_dump_dir.empty()) {
        maybe_write_bin_vec(debug_dump_dir, "cpp_window_video.f64", window->video);
        maybe_write_bin_vec(debug_dump_dir, "cpp_window_video05.f64", window->video_05);
        maybe_write_bin_vec(debug_dump_dir, "cpp_window_input.f64", window->input);
        maybe_write_bin_vec(debug_dump_dir, "cpp_window_chroma.f64", window->chroma);
    }

    const auto t_k2_0 = std::chrono::steady_clock::now();
    vhsdecode_cpp::ResyncFieldInput fin{};
    fin.demod = window->video;
    fin.demod_05 = window->video_05;
    fin.color_system = "NTSC";
    fin.fallback_vsync = false;
    fin.disable_dc_offset = false;
    auto rawpulses = resync.get_pulses(fin, true);
    if (debug_dump_this_field && !debug_dump_dir.empty()) {
        maybe_write_bin_vec(debug_dump_dir, "cpp_window_video_resync.f64", fin.demod);
        maybe_write_bin_vec(debug_dump_dir, "cpp_window_video05_resync.f64", fin.demod_05);
        std::ostringstream rawcsv;
        rawcsv << "start,len\n";
        for (const auto& p : rawpulses) rawcsv << p.start << "," << p.len << "\n";
        maybe_write_text(debug_dump_dir, "cpp_rawpulses.csv", rawcsv.str());
    }
    DecodedField out{};
    out.resync_last_pulse_threshold = resync.last_pulse_threshold();
    {
        const auto [sync, blank] = resync.field_state_levels();
        out.resync_field_sync = sync;
        out.resync_field_blank = blank;
    }
    if (const auto serr = resync.serration_levels(); serr.has_value()) {
        out.resync_serration_sync = serr->first;
        out.resync_serration_blank = serr->second;
    }
    out.resync_sync_level_bias = resync.sync_level_bias();
    out.raw_pulses = static_cast<int>(rawpulses.size());
    if (rawpulses.empty()) {
        out.valid = false;
        out.next_offset = static_cast<int>(k1_cfg.freq_hz / 10.0);
        out.invalid_reason = "no_raw_pulses";
        perf.k2_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_k2_0).count();
        return out;
    }

    const auto timings = compute_timings(rawpulses, k1_cfg.freq_hz / 1.0e6);
    std::vector<vhsdecode_cpp::RawPulse> rawpulses_refine;
    rawpulses_refine.reserve(rawpulses.size());
    for (const auto& p : rawpulses) rawpulses_refine.push_back({p.start, p.len});
    auto validpulses = vhsdecode_cpp::refine_pulses(
        rawpulses_refine,
        fin.demod_05,
        timings,
        6,
        rf_linelen_i,
        field_cfg.input.luma.ire0,
        field_cfg.input.luma.hz_ire,
        static_cast<double>(resync.eq_pulselen()),
        static_cast<double>(resync.long_pulse_max()));
    out.valid_pulses = static_cast<int>(validpulses.size());
    if (debug_dump_this_field && !debug_dump_dir.empty()) {
        std::ostringstream vpcsv;
        vpcsv << "type,start\n";
        for (const auto& p : validpulses) vpcsv << p.type << "," << p.start << "\n";
        maybe_write_text(debug_dump_dir, "cpp_validpulses.csv", vpcsv.str());
    }
    if (validpulses.empty()) {
        out.valid = false;
        out.next_offset = rf_linelen_i * 100;
        out.invalid_reason = "no_valid_pulses";
        perf.k2_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_k2_0).count();
        return out;
    }
    const double meanlinelen = compute_meanlinelen(validpulses, static_cast<double>(rf_linelen_i));
    vhsdecode_cpp::FirstHsyncInput first{};
    first.validpulses = validpulses;
    first.meanlinelen = meanlinelen;
    first.is_ntsc = true;
    // LOUD FAITHFUL-PORT NOTE:
    // Upstream NTSC field_lines is [263, 262] (first field, second field).
    // Reversing this silently shifts second-field next-field projection by one
    // full line while still leaving the local first-HSYNC anchor looking
    // plausible, which is exactly what skewed the startup scheduler and the
    // first written native field.
    first.field_lines = {263, 262};
    first.num_eq_pulses = 6;
    first.prev_first_field = runtime.prev_first_field;
    first.last_field_offset_lines =
        (runtime.prev_first_hsync_readloc != -1)
            ? (static_cast<double>(runtime.prev_first_hsync_readloc) - static_cast<double>(readloc)) / meanlinelen
            : 0.0;
    first.prev_first_hsync_loc = runtime.prev_first_hsync_loc;
    first.prev_hsync_diff = runtime.prev_hsync_diff;
    first.field_order_confidence = 0;
    first.fallback_line0loc = -1.0;
    first.fallback_is_first_field = -1;
    first.fallback_is_first_field_confidence = -1;
    const auto first_res = vhsdecode_cpp::get_first_hsync_loc(first);
    out.has_first_hsync = first_res.has_first_hsync_loc;
    if (debug_dump_this_field && !debug_dump_dir.empty()) {
        nlohmann::json j;
        j["readloc"] = static_cast<int>(readloc);
        j["meanlinelen"] = meanlinelen;
        j["prev_first_field"] = runtime.prev_first_field;
        j["last_field_offset_lines"] = first.last_field_offset_lines;
        j["prev_first_hsync_loc"] = runtime.prev_first_hsync_loc;
        j["prev_hsync_diff"] = runtime.prev_hsync_diff;
        j["has_first_hsync_loc"] = first_res.has_first_hsync_loc;
        j["first_hsync_loc"] = first_res.first_hsync_loc;
        j["hsync_start_line"] = first_res.hsync_start_line;
        j["line0loc"] = first_res.line0loc;
        j["next_field"] = first_res.next_field;
        j["first_field"] = first_res.first_field;
        j["first_field_confidence"] = first_res.first_field_confidence;
        j["second_field_confidence"] = first_res.second_field_confidence;
        j["progressive_field_confidence"] = first_res.progressive_field_confidence;
        j["field_order_lengths"] = first_res.field_order_lengths;
        j["prev_hsync_diff_out"] = first_res.prev_hsync_diff;
        j["hsync_threshold"] = resync.last_pulse_threshold();
        j["normal_hsync_length"] = static_cast<int>((k1_cfg.freq_hz / 1.0e6) * 4.7);
        j["one_usec"] = static_cast<int>(k1_cfg.freq_hz / 1.0e6);
        maybe_write_text(debug_dump_dir, "cpp_first_hsync.json", j.dump(2) + "\n");
    }
    if (!first_res.has_first_hsync_loc) {
        out.valid = false;
        out.next_offset = rf_linelen_i * 100;
        out.invalid_reason = "no_first_hsync";
        perf.k2_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_k2_0).count();
        return out;
    }

    const int proclines = field_cfg.input.luma.outlinecount + field_cfg.input.luma.lineoffset + 10;
    const double line0loc = first_res.line0loc;
    const double lastline = (static_cast<double>(window->input.size()) - line0loc) / meanlinelen - 1.0;
    if (lastline < static_cast<double>(proclines)) {
        out.valid = false;
        out.next_offset = static_cast<int>(std::max(line0loc - (meanlinelen * 20.0), static_cast<double>(rf_linelen_i)));
        out.invalid_reason = "insufficient_tail";
        perf.k2_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_k2_0).count();
        return out;
    }

    std::vector<double> pulse_starts;
    pulse_starts.reserve(validpulses.size());
    for (const auto& p : validpulses) pulse_starts.push_back(static_cast<double>(p.start));
    auto lineloc0 = vhsdecode_cpp::valid_pulses_to_linelocs(
        pulse_starts,
        static_cast<int>(std::lround(first_res.first_hsync_loc)),
        static_cast<int>(std::lround(first_res.hsync_start_line)),
        meanlinelen,
        0.8,
        proclines,
        1.9);
    std::vector<std::uint8_t> linebad = lineloc0.line_location_errs;
    // LOUD FAITHFUL-PORT NOTE:
    // VHS FieldShared.compute_deriv_error() is intentionally a no-op.
    // Earlier driver bring-up added an lddecode-style second-derivative mask
    // here, but that is not part of the active upstream VHS live path and can
    // mark extra lines bad before burst refine/downscale. Keep the raw
    // valid_pulses_to_linelocs() error mask unless upstream VHS changes.

    vhsdecode_cpp::HsyncRefineInput refine{};
    refine.linelocs1 = lineloc0.line_locations;
    refine.demod_05 = fin.demod_05;
    refine.normal_hsync_length = static_cast<int>((k1_cfg.freq_hz / 1.0e6) * 4.7);
    refine.one_usec = static_cast<int>(k1_cfg.freq_hz / 1.0e6);
    refine.sample_rate_mhz = k1_cfg.freq_hz / 1.0e6;
    refine.is_pal = false;
    refine.disable_right_hsync = false;
    // LOUD FAITHFUL-PORT NOTE:
    // The active upstream NTSC VHS path uses rf.resync.last_pulse_threshold
    // during HSYNC refine (hsync_refine_use_threshold=True), not the static
    // iretohz(vsync_ire/2) fallback. That small threshold delta is enough to
    // leave a few-sample RMS lineloc error, which then blows up downstream
    // luma fidelity.
    refine.hsync_threshold = resync.last_pulse_threshold();
    refine.ire_30 = ire_to_hz_local(field_cfg.input.luma.ire0, field_cfg.input.luma.hz_ire, 30.0);
    refine.ire_n_65 = ire_to_hz_local(field_cfg.input.luma.ire0, field_cfg.input.luma.hz_ire, -65.0);
    refine.ire_110 = ire_to_hz_local(field_cfg.input.luma.ire0, field_cfg.input.luma.hz_ire, 110.0);
    auto linelocs = vhsdecode_cpp::refine_linelocs_hsync(refine, linebad);
    if (debug_dump_this_field && !debug_dump_dir.empty()) {
        maybe_write_bin_vec(debug_dump_dir, "cpp_linelocs0.f64", lineloc0.line_locations);
        maybe_write_bin_vec(debug_dump_dir, "cpp_lineloc_errs.u8", lineloc0.line_location_errs);
        maybe_write_bin_vec(debug_dump_dir, "cpp_linelocs1.f64", lineloc0.line_locations);
        maybe_write_bin_vec(debug_dump_dir, "cpp_linelocs2.f64", linelocs);
        maybe_write_bin_vec(debug_dump_dir, "cpp_linebad.u8", linebad);
    }
    perf.k2_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_k2_0).count();

    runtime.prev_first_hsync_readloc = static_cast<int>(readloc);
    runtime.prev_first_hsync_loc = first_res.first_hsync_loc;
    runtime.prev_hsync_diff = first_res.prev_hsync_diff;
    runtime.prev_first_field = first_res.first_field ? 1 : 0;

    const double nextfield = first_res.has_next_field
        ? (first_res.next_field - (static_cast<double>(rf_linelen_i) * 8.0))
        : lineloc0.line_locations[static_cast<std::size_t>(field_cfg.input.luma.outlinecount - 7)];

    const auto t_k3_0 = std::chrono::steady_clock::now();
    auto chroma_scale_input = field_cfg.input.luma;
    chroma_scale_input.inlinelen = static_cast<double>(rf_linelen_i);
    chroma_scale_input.demod.assign(window->chroma.begin(), window->chroma.end());
    chroma_scale_input.linelocs = linelocs;
    chroma_scale_input.final_output = false;
    chroma_scale_input.y_comb_limit = 0.0;
    auto chroma_downscaled_phase = vhsdecode_cpp::downscale_luma(chroma_scale_input).dsout_float;

    auto phase_input = phase_cfg.input;
    phase_input.chroma_rotation_index = runtime.track_phase;
    phase_input.chroma.assign(chroma_downscaled_phase.begin(), chroma_downscaled_phase.end());
    const auto phase_result = vhsdecode_cpp::get_phase_rotation_sequence(phase_input);
    runtime.track_phase = phase_result.track_phase;
    perf.k3_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_k3_0).count();

    // LOUD FAITHFUL-PORT NOTE:
    // Active NTSC VHS does not feed raw linelocs2 straight into final output.
    // Upstream FieldNTSCShared/FieldNTSC still performs:
    // 1. refine_linelocs_burst() via burst phase lock
    // 2. fix_badlines(..., linelocs2)
    // 3. apply_offsets(..., -83 degrees)
    // before the final luma/chroma downscale path. Skipping that sequence left
    // the live decoder on a different field-local geometry contract than
    // upstream even when K1/K2 were otherwise tight.
    auto final_linelocs = linelocs;
    if (phase_result.burst_phase_avg.has_value() && !phase_result.phase_sequence.empty()) {
        sync_to_burst(
            final_linelocs,
            linebad,
            field_cfg.input.luma.outlinelen,
            *phase_result.burst_phase_avg,
            phase_result.phase_sequence);
    }
    if (debug_dump_this_field && !debug_dump_dir.empty()) {
        maybe_write_bin_vec(debug_dump_dir, "cpp_linelocs3.f64", final_linelocs);
    }
    fix_badlines(final_linelocs, linelocs, linebad, field_cfg.input.luma.lineoffset);
    if (debug_dump_this_field && !debug_dump_dir.empty()) {
        maybe_write_bin_vec(debug_dump_dir, "cpp_linelocs4.f64", final_linelocs);
    }
    apply_ntsc_phase_offset(final_linelocs, k1_cfg.freq_hz / 1.0e6);
    if (debug_dump_this_field && !debug_dump_dir.empty()) {
        maybe_write_bin_vec(debug_dump_dir, "cpp_linelocs_final.f64", final_linelocs);
    }

    auto live_luma_params = field_cfg.input.luma;
    const auto detected_levels = detect_levels_ntsc(
        fin.demod,
        fin.demod_05,
        linelocs,
        k1_cfg.freq_hz / 1.0e6,
        field_cfg.input.luma.ire0,
        field_cfg.input.luma.hz_ire,
        field_cfg.input.luma.vsync_ire,
        field_cfg.input.luma.outlinecount);
    if (detected_levels.valid) {
        live_luma_params.ire0 = detected_levels.ire0;
        live_luma_params.hz_ire = detected_levels.hz_ire;
        live_luma_params.vsync_ire = detected_levels.vsync_ire;
        live_luma_params.out_scale = detected_levels.out_scale;
    }

    auto luma_input = live_luma_params;
    luma_input.inlinelen = static_cast<double>(rf_linelen_i);
    // LOUD FAITHFUL-PORT NOTE:
    // Upstream Field.downscale(channel="demod") uses the field object's
    // stored `data["video"]["demod"]` substrate. On the real live path that
    // still matches the original demodcache window, not the temporary demod
    // buffer that Resync/get_pulses mutates while hunting sync. Feeding the
    // mutated resync buffer into K4 produces a nearly pure constant negative
    // luma offset (~56 kHz on the first written native field) while geometry
    // and chroma still look healthy, which is exactly the "VHSpp is darker"
    // failure seen in the finished TBCs.
    luma_input.demod.assign(window->video.begin(), window->video.end());
    luma_input.linelocs = final_linelocs;

    auto chroma_scale_final_input = field_cfg.input.luma;
    chroma_scale_final_input.inlinelen = static_cast<double>(rf_linelen_i);
    chroma_scale_final_input.demod.assign(window->chroma.begin(), window->chroma.end());
    chroma_scale_final_input.linelocs = final_linelocs;
    chroma_scale_final_input.final_output = false;
    chroma_scale_final_input.y_comb_limit = 0.0;
    auto chroma_downscaled_final = vhsdecode_cpp::downscale_luma(chroma_scale_final_input).dsout_float;

    const auto t_k4_0 = std::chrono::steady_clock::now();
    auto full_input = field_cfg.input;
    full_input.luma = luma_input;
    full_input.luma.linelocs = final_linelocs;
    full_input.chroma.chroma.assign(chroma_downscaled_final.begin(), chroma_downscaled_final.end());
    full_input.chroma.phase_sequence = phase_result.phase_sequence;
    full_input.chroma.burst_phase_avg = phase_result.burst_phase_avg;
    const auto field_out = vhsdecode_cpp::downscale_ntsc_vhs(full_input);
    perf.k4_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_k4_0).count();

    out.valid = true;
    out.is_first_field = first_res.first_field;
    // LOUD FAITHFUL-PORT NOTE:
    // Upstream Field.readloc/fileLoc is the demod-cache window origin
    // (`rawdecode["startloc"]`), not the scheduler's `start`/`fdoffset`
    // request. Keeping the request offset here makes image comparisons look
    // good while metadata/session fidelity drifts by about one cache block.
    out.readloc = static_cast<int>(window->startloc);
    out.next_offset = static_cast<int>(std::llround(
        nextfield - (static_cast<double>(readloc) - static_cast<double>(window->startloc))));
    out.linelocs = final_linelocs;
    out.sync_conf = compute_sync_confidence(
        out.linelocs,
        field_cfg.input.luma.lineoffset,
        field_cfg.input.luma.outlinecount,
        100);
    out.linebad = std::move(linebad);
    out.detected_levels = detected_levels;
    out.output_ire0 = live_luma_params.ire0;
    out.output_hz_ire = live_luma_params.hz_ire;
    out.output_vsync_ire = live_luma_params.vsync_ire;
    out.output_out_scale = live_luma_params.out_scale;
    out.luma_float = field_out.luma.dsout_float;
    out.luma_u16 = field_out.luma.dsout_u16;
    out.chroma_u16 = field_out.chroma.chroma_u16;
    return out;
}

nlohmann::json metadata_json_from(const vhsdecode_cpp::MetadataOutput& m) {
    nlohmann::json j;
    j["isFirstField"] = m.is_first_field;
    j["syncConf"] = m.sync_conf;
    j["seqNo"] = m.seq_no;
    j["diskLoc"] = m.disk_loc;
    j["fileLoc"] = m.file_loc;
    j["fieldPhaseID"] = m.field_phase_id;
    if (m.decode_faults.has_value()) j["decodeFaults"] = *m.decode_faults;
    return j;
}

vhsdecode_cpp::DecodeDriverConfig parse_args(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error(
            "usage: vhsdecode_ntsc_vhs_decode <input_u8> <output_base> "
            "[--profile ntsc-vhs] [--metadata-json FILE] [--k1-dir DIR] "
            "[--k3-phase-dir DIR] [--k4-dir DIR] [--debug-dump-dir DIR] "
            "[--debug-capture-seq N] [--max-fields N] [--max-attempts N]");
    }
    vhsdecode_cpp::DecodeDriverConfig out;
    out.profile = vhsdecode_cpp::DecodeProfile::NtscVhs;
    out.input_u8 = argv[1];
    out.output_base = argv[2];
    out.k1_dir = "/media/hunter/DATA/captures/k1_parity_native_nr";
    out.k3_phase_dir = "/media/hunter/DATA/captures/k3_phase_native_field900";
    out.k4_dir = "/media/hunter/DATA/captures/k4_field_native_live";
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--profile" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "ntsc-vhs") out.profile = vhsdecode_cpp::DecodeProfile::NtscVhs;
            else throw std::runtime_error("unsupported profile: " + value);
        } else if (arg == "--k1-dir" && i + 1 < argc) out.k1_dir = argv[++i];
        else if (arg == "--k3-phase-dir" && i + 1 < argc) out.k3_phase_dir = argv[++i];
        else if (arg == "--k4-dir" && i + 1 < argc) out.k4_dir = argv[++i];
        else if (arg == "--metadata-json" && i + 1 < argc) out.metadata_json = argv[++i];
        else if (arg == "--debug-dump-dir" && i + 1 < argc) out.debug_dump_dir = argv[++i];
        else if (arg == "--debug-capture-seq" && i + 1 < argc) out.debug_capture_seq = std::stoi(argv[++i]);
        else if (arg == "--max-fields" && i + 1 < argc) out.max_fields = std::stoi(argv[++i]);
        else if (arg == "--max-attempts" && i + 1 < argc) out.max_attempts = std::stoi(argv[++i]);
        else throw std::runtime_error("unknown argument: " + arg);
    }
    return out;
}

}  // namespace

namespace vhsdecode_cpp {

DecodeDriverResult run_decode_driver(const DecodeDriverConfig& args) {
        const auto k1_kv = read_kv(args.k1_dir / "config.kv");
        const auto k1_cfg = load_k1_config(k1_kv);
        const std::size_t blockcut = parse_num<std::size_t>(k1_kv, "blockcut");
        const std::size_t blockcut_end = parse_num<std::size_t>(k1_kv, "blockcut_end");
        auto k1_ctx = build_k1_context(k1_cfg);
        const auto phase_cfg = load_phase_config(args.k3_phase_dir);
        auto field_cfg = load_field_config(args.k4_dir);
        // LOUD FAITHFUL-PORT NOTE:
        // The saved K3 parity fixture may contain a field-specific `track_phase`
        // value from whatever field produced that artifact. Upstream live decode
        // does NOT seed `rf.track_phase` from an old fixture; it starts from the
        // runtime/default state unless the user explicitly forces `--track_phase`.
        // Leaking a baked fixture value here can flip the live NTSC chroma phase
        // by 180 degrees while leaving the rest of the driver looking healthy.
        auto live_phase_cfg = phase_cfg;
        live_phase_cfg.input.chroma_rotation_index.reset();
        vhsdecode_cpp::ResyncRuntimeConfig rcfg{};
        rcfg.sample_rate_hz = k1_cfg.freq_hz;
        rcfg.sample_rate_mhz = k1_cfg.freq_hz / 1.0e6;
        rcfg.divisor = 1;
        rcfg.fps = k1_cfg.fps;
        rcfg.frame_lines = k1_cfg.frame_lines;
        rcfg.eq_pulse_us = 2.3;
        rcfg.vsync_pulse_us = 27.1;
        rcfg.sysparams_const.ire0 = field_cfg.input.luma.ire0;
        rcfg.sysparams_const.hz_ire = field_cfg.input.luma.hz_ire;
        rcfg.sysparams_const.vsync_hz = ire_to_hz_local(field_cfg.input.luma.ire0, field_cfg.input.luma.hz_ire, field_cfg.input.luma.vsync_ire);
        vhsdecode_cpp::ResyncRuntime resync(rcfg);

        vhsdecode_cpp::DemodCache reader(args.input_u8, k1_cfg, std::move(k1_ctx), blockcut, blockcut_end);

        std::ofstream tbc_out(args.output_base.string() + ".tbc", std::ios::binary);
        std::ofstream chroma_out(args.output_base.string() + "_chroma.tbc", std::ios::binary);
        if (!tbc_out.is_open() || !chroma_out.is_open()) {
            throw std::runtime_error("failed to open output files");
        }

        vhsdecode_cpp::MetadataState meta_state;
        vhsdecode_cpp::SessionState session_state{};
        RuntimeState runtime{};
        DriverPerfStats perf{};
        struct StagedField {
            DecodedField field;
            vhsdecode_cpp::MetadataOutput metadata;
            nlohmann::json field_json;
        };
        std::optional<StagedField> lastvalid_false;
        std::optional<StagedField> lastvalid_true;
        std::vector<std::uint64_t> guided_starts;
        if (!args.metadata_json.empty()) {
            std::ifstream min(args.metadata_json);
            if (!min.is_open()) throw std::runtime_error("failed to open metadata json: " + args.metadata_json.string());
            nlohmann::json meta;
            min >> meta;
            const auto& fields = meta.is_object() ? meta.at("fields") : meta;
            for (const auto& f : fields) {
                if (f.contains("fileLoc")) {
                    guided_starts.push_back(static_cast<std::uint64_t>(f.at("fileLoc").get<int>()) + static_cast<std::uint64_t>(blockcut));
                }
            }
        }

        std::uint64_t fdoffset = 0;
        int fields_written = 0;
        int fields_seen = 0;
        int attempts = 0;
        bool dumped_debug_field = false;
        auto t0 = std::chrono::steady_clock::now();

        while (true) {
            if (args.max_fields > 0 && fields_seen >= args.max_fields) break;
            if (args.max_attempts > 0 && attempts >= args.max_attempts) break;
            ++attempts;
            const std::uint64_t decode_start =
                (!guided_starts.empty() && static_cast<std::size_t>(attempts - 1) < guided_starts.size())
                    ? guided_starts[static_cast<std::size_t>(attempts - 1)]
                    : fdoffset;
            const auto field = decode_field(
                decode_start,
                reader,
                blockcut,
                k1_cfg,
                resync,
                live_phase_cfg,
                field_cfg,
                runtime,
                perf,
                args.debug_dump_dir,
                !args.debug_dump_dir.empty() &&
                    !dumped_debug_field &&
                    !guided_starts.empty() &&
                    attempts == args.debug_capture_seq);
            if (!field.has_value()) break;
            if (!field->valid) {
                if (attempts <= 10 || (attempts % 25) == 0) {
                    std::cerr << "attempt=" << attempts
                              << " readloc=" << decode_start
                              << " raw_pulses=" << field->raw_pulses
                              << " valid_pulses=" << field->valid_pulses
                              << " has_first_hsync=" << (field->has_first_hsync ? 1 : 0)
                              << " reason=" << field->invalid_reason
                              << " next_offset=" << field->next_offset
                              << "\n";
                }
                if (guided_starts.empty()) {
                    if (field->next_offset <= 0) break;
                    fdoffset += static_cast<std::uint64_t>(field->next_offset);
                    if (fdoffset >= reader.file_size()) break;
                } else if (static_cast<std::size_t>(attempts) >= guided_starts.size()) {
                    break;
                }
                continue;
            }

            ++fields_seen;
            if (fields_seen <= 10 || (fields_seen % 25) == 0) {
                std::cerr << "field=" << fields_seen
                          << " attempts=" << attempts
                          << " readloc=" << field->readloc
                          << " is_first=" << (field->is_first_field ? 1 : 0)
                          << " next_offset=" << field->next_offset
                          << "\n";
            }

            // VHSDecode.readfield(): skip a leading second field before
            // buildmetadata() so seqNo/fieldPhaseID/history stay aligned with
            // actually written output fields.
            if (session_state.output_state.fieldinfo.empty() && !field->is_first_field) {
                fdoffset += static_cast<std::uint64_t>(field->next_offset);
                continue;
            }

            vhsdecode_cpp::MetadataInput mi{};
            mi.is_first_field = field->is_first_field;
            mi.detected_first_field = field->is_first_field;
            mi.sync_conf = field->sync_conf;
            mi.seq_no = static_cast<int>(meta_state.fieldinfo.size()) + 1;
            const double bytes_per_field = k1_cfg.freq_hz / (k1_cfg.fps * 2.0);
            mi.disk_loc = std::round((static_cast<double>(field->readloc) / bytes_per_field) * 10.0) / 10.0;
            mi.file_loc = field->readloc;
            const auto mo = vhsdecode_cpp::build_metadata(mi, meta_state);
            const auto fi = metadata_json_from(mo);

            if (field->readloc >= 137000000ULL && field->readloc <= 141500000ULL) {
                std::cerr << "meta readloc=" << field->readloc
                          << " in_is_first=" << (field->is_first_field ? 1 : 0)
                          << " sync_conf=" << field->sync_conf
                          << " seq_no=" << mo.seq_no
                          << " out_is_first=" << (mo.is_first_field ? 1 : 0)
                          << " dup=" << (mo.is_duplicate_field ? 1 : 0)
                          << " write=" << (mo.write_field ? 1 : 0)
                          << " phase=" << mo.field_phase_id
                          << " decode_faults="
                          << (mo.decode_faults.has_value() ? std::to_string(*mo.decode_faults) : std::string("none"))
                          << "\n";
            }

            if (mo.write_field) {
                StagedField staged{*field, mo, fi};
                if (field->is_first_field) lastvalid_true = staged;
                else lastvalid_false = staged;
            }

            if (mo.is_duplicate_field) {
                const auto& other = field->is_first_field ? lastvalid_false : lastvalid_true;
                const auto& self = field->is_first_field ? lastvalid_true : lastvalid_false;
                if (other.has_value()) {
                    if (!args.debug_dump_dir.empty() && !dumped_debug_field &&
                        (fields_written + 1) == args.debug_capture_seq) {
                        fs::create_directories(args.debug_dump_dir);
                        write_bin_vec(args.debug_dump_dir / "cpp_linelocs.f64", other->field.linelocs);
                        write_bin_vec(args.debug_dump_dir / "cpp_luma_float.f32", other->field.luma_float);
                        write_bin_vec(args.debug_dump_dir / "cpp_luma_u16.u16", other->field.luma_u16);
                        write_bin_vec(args.debug_dump_dir / "cpp_chroma_u16.u16", other->field.chroma_u16);
                        nlohmann::json dj;
                        dj["readloc"] = other->field.readloc;
                        dj["is_first_field"] = other->field.is_first_field;
                        dj["next_offset"] = other->field.next_offset;
                        dj["sync_conf"] = other->field.sync_conf;
                        dj["resync"] = nlohmann::json{
                            {"last_pulse_threshold", other->field.resync_last_pulse_threshold},
                            {"field_sync", other->field.resync_field_sync.has_value() ? nlohmann::json(*other->field.resync_field_sync) : nlohmann::json(nullptr)},
                            {"field_blank", other->field.resync_field_blank.has_value() ? nlohmann::json(*other->field.resync_field_blank) : nlohmann::json(nullptr)},
                            {"serration_sync", other->field.resync_serration_sync.has_value() ? nlohmann::json(*other->field.resync_serration_sync) : nlohmann::json(nullptr)},
                            {"serration_blank", other->field.resync_serration_blank.has_value() ? nlohmann::json(*other->field.resync_serration_blank) : nlohmann::json(nullptr)},
                            {"sync_level_bias", other->field.resync_sync_level_bias},
                        };
                        dj["detected_levels"] = nlohmann::json{
                            {"valid", other->field.detected_levels.valid},
                            {"ire0", other->field.detected_levels.ire0},
                            {"hz_ire", other->field.detected_levels.hz_ire},
                            {"vsync_ire", other->field.detected_levels.vsync_ire},
                            {"out_scale", other->field.detected_levels.out_scale},
                        };
                        dj["output_levels"] = nlohmann::json{
                            {"ire0", other->field.output_ire0},
                            {"hz_ire", other->field.output_hz_ire},
                            {"vsync_ire", other->field.output_vsync_ire},
                            {"out_scale", other->field.output_out_scale},
                        };
                        std::ofstream jout(args.debug_dump_dir / "cpp_field.json");
                        jout << dj.dump(2) << "\n";
                        dumped_debug_field = true;
                    }
                    const auto t_w0 = std::chrono::steady_clock::now();
                    write_bin_vec_once(fs::path(args.output_base.string() + ".firstfield.linelocs.f64"), other->field.linelocs);
                    write_bin_vec_once(fs::path(args.output_base.string() + ".firstfield.linebad.u8"), other->field.linebad);
                    write_u16(tbc_out, other->field.luma_u16);
                    write_u16(chroma_out, other->field.chroma_u16);
                    session_state.output_state.fieldinfo.push_back(other->field_json);
                    session_state.output_state.fields_written += 1;
                    session_state.output_state.video_bytes_written +=
                        other->field.luma_u16.size() * sizeof(std::uint16_t);
                    session_state.output_state.chroma_bytes_written +=
                        other->field.chroma_u16.size() * sizeof(std::uint16_t);
                    meta_state.fieldinfo.push_back(other->metadata);
                    ++fields_written;
                    if (self.has_value()) {
                        write_u16(tbc_out, self->field.luma_u16);
                        write_u16(chroma_out, self->field.chroma_u16);
                        session_state.output_state.fieldinfo.push_back(self->field_json);
                        session_state.output_state.fields_written += 1;
                        session_state.output_state.video_bytes_written +=
                            self->field.luma_u16.size() * sizeof(std::uint16_t);
                        session_state.output_state.chroma_bytes_written +=
                            self->field.chroma_u16.size() * sizeof(std::uint16_t);
                        meta_state.fieldinfo.push_back(self->metadata);
                        ++fields_written;
                    }
                    perf.write_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_w0).count();
                }
            } else if (mo.write_field) {
                const auto& staged = field->is_first_field ? lastvalid_true : lastvalid_false;
                if (staged.has_value()) {
                    if (!args.debug_dump_dir.empty() && !dumped_debug_field &&
                        (fields_written + 1) == args.debug_capture_seq) {
                        fs::create_directories(args.debug_dump_dir);
                        write_bin_vec(args.debug_dump_dir / "cpp_linelocs.f64", staged->field.linelocs);
                        write_bin_vec(args.debug_dump_dir / "cpp_luma_float.f32", staged->field.luma_float);
                        write_bin_vec(args.debug_dump_dir / "cpp_luma_u16.u16", staged->field.luma_u16);
                        write_bin_vec(args.debug_dump_dir / "cpp_chroma_u16.u16", staged->field.chroma_u16);
                        nlohmann::json dj;
                        dj["readloc"] = staged->field.readloc;
                        dj["is_first_field"] = staged->field.is_first_field;
                        dj["next_offset"] = staged->field.next_offset;
                        dj["sync_conf"] = staged->field.sync_conf;
                        dj["resync"] = nlohmann::json{
                            {"last_pulse_threshold", staged->field.resync_last_pulse_threshold},
                            {"field_sync", staged->field.resync_field_sync.has_value() ? nlohmann::json(*staged->field.resync_field_sync) : nlohmann::json(nullptr)},
                            {"field_blank", staged->field.resync_field_blank.has_value() ? nlohmann::json(*staged->field.resync_field_blank) : nlohmann::json(nullptr)},
                            {"serration_sync", staged->field.resync_serration_sync.has_value() ? nlohmann::json(*staged->field.resync_serration_sync) : nlohmann::json(nullptr)},
                            {"serration_blank", staged->field.resync_serration_blank.has_value() ? nlohmann::json(*staged->field.resync_serration_blank) : nlohmann::json(nullptr)},
                            {"sync_level_bias", staged->field.resync_sync_level_bias},
                        };
                        dj["detected_levels"] = nlohmann::json{
                            {"valid", staged->field.detected_levels.valid},
                            {"ire0", staged->field.detected_levels.ire0},
                            {"hz_ire", staged->field.detected_levels.hz_ire},
                            {"vsync_ire", staged->field.detected_levels.vsync_ire},
                            {"out_scale", staged->field.detected_levels.out_scale},
                        };
                        dj["output_levels"] = nlohmann::json{
                            {"ire0", staged->field.output_ire0},
                            {"hz_ire", staged->field.output_hz_ire},
                            {"vsync_ire", staged->field.output_vsync_ire},
                            {"out_scale", staged->field.output_out_scale},
                        };
                        std::ofstream jout(args.debug_dump_dir / "cpp_field.json");
                        jout << dj.dump(2) << "\n";
                        dumped_debug_field = true;
                    }
                    const auto t_w0 = std::chrono::steady_clock::now();
                    write_bin_vec_once(fs::path(args.output_base.string() + ".firstfield.linelocs.f64"), staged->field.linelocs);
                    write_bin_vec_once(fs::path(args.output_base.string() + ".firstfield.linebad.u8"), staged->field.linebad);
                    write_u16(tbc_out, staged->field.luma_u16);
                    write_u16(chroma_out, staged->field.chroma_u16);
                    session_state.output_state.fieldinfo.push_back(staged->field_json);
                    session_state.output_state.fields_written += 1;
                    session_state.output_state.video_bytes_written +=
                        staged->field.luma_u16.size() * sizeof(std::uint16_t);
                    session_state.output_state.chroma_bytes_written +=
                        staged->field.chroma_u16.size() * sizeof(std::uint16_t);
                    meta_state.fieldinfo.push_back(staged->metadata);
                    ++fields_written;
                    perf.write_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_w0).count();
                }
            }

            if (guided_starts.empty()) {
                if (field->next_offset <= 0) break;
                fdoffset += static_cast<std::uint64_t>(field->next_offset);
                if (fdoffset >= reader.file_size()) break;
            } else if (static_cast<std::size_t>(attempts) >= guided_starts.size()) {
                break;
            }
        }

        vhsdecode_cpp::WriteoutState wstate = session_state.output_state;
        vhsdecode_cpp::BuildJsonInput j{};
        j.analog_audio = 0;
        j.os_info = "cpp_port";
        j.git_branch = "unknown";
        j.git_commit = "unknown";
        j.system = "NTSC";
        j.field_width = field_cfg.input.luma.outlinelen;
        j.sample_rate = k1_cfg.fsc_mhz * 4.0 * 1000000.0;
        // Match lddecode.core.build_json(): for NTSC CLI decode, blackIRE is 7.5.
        const double black_ire = 7.5;
        const double vsync_ire = field_cfg.input.luma.vsync_ire;
        j.black16b_ire = field_cfg.input.luma.output_zero + ((black_ire - vsync_ire) * field_cfg.input.luma.out_scale);
        j.white16b_ire = field_cfg.input.luma.output_zero + ((100.0 - vsync_ire) * field_cfg.input.luma.out_scale);
        j.field_height = field_cfg.input.luma.outlinecount;
        // Match lddecode.core.build_json() timing metadata rather than replay-artifact placeholders.
        constexpr double badj = -1.4;
        const double spu = k1_cfg.fsc_mhz * 4.0;
        j.colour_burst_start = static_cast<int>(std::llround((5.3 * spu) + badj));
        j.colour_burst_end = static_cast<int>(std::llround((7.8 * spu) + badj));
        j.active_video_start = static_cast<int>(std::llround((9.45 * spu) + badj));
        j.active_video_end = static_cast<int>(std::llround(((1.0 / (k1_cfg.fsc_mhz / 227.5)) - 1.0) * spu + badj));
        j.level_adjust = 0.1;
        j.color_system = "NTSC";
        j.tape_format = "VHS";
        auto json_out = vhsdecode_cpp::build_vhs_json(j, wstate);
        json_out["fields"] = wstate.fieldinfo;
        std::ofstream jout(args.output_base.string() + ".tbc.json");
        jout << std::setw(2) << json_out << "\n";

        const auto t1 = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(t1 - t0).count();
        const double fps_fields = elapsed > 0.0 ? static_cast<double>(fields_written) / elapsed : 0.0;
        const double fps_frames = elapsed > 0.0 ? (static_cast<double>(fields_written) / 2.0) / elapsed : 0.0;

        DecodeDriverResult result{};
        result.input = args.input_u8.string();
        result.output_base = args.output_base.string();
        result.fields_seen = fields_seen;
        result.attempts = attempts;
        result.fields_written = fields_written;
        result.elapsed_s = elapsed;
        result.throughput_fields_per_s = fps_fields;
        result.throughput_frames_per_s = fps_frames;
        std::cerr << std::fixed << std::setprecision(6)
                  << "perf read_s=" << perf.read_s
                  << " k2_s=" << perf.k2_s
                  << " k3_s=" << perf.k3_s
                  << " k4_s=" << perf.k4_s
                  << " write_s=" << perf.write_s
                  << "\n";
        return result;
}

}  // namespace vhsdecode_cpp

int main(int argc, char** argv) {
    try {
        const auto args = parse_args(argc, argv);
        const auto result = vhsdecode_cpp::run_decode_driver(args);
        std::cout << std::setprecision(17)
                  << "{\n"
                  << "  \"input\": \"" << result.input << "\",\n"
                  << "  \"output_base\": \"" << result.output_base << "\",\n"
                  << "  \"fields_seen\": " << result.fields_seen << ",\n"
                  << "  \"attempts\": " << result.attempts << ",\n"
                  << "  \"fields_written\": " << result.fields_written << ",\n"
                  << "  \"elapsed_s\": " << result.elapsed_s << ",\n"
                  << "  \"throughput_fields_per_s\": " << result.throughput_fields_per_s << ",\n"
                  << "  \"throughput_frames_per_s\": " << result.throughput_frames_per_s << "\n"
                  << "}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}

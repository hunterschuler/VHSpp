#include "vhsdecode_cpp/k1_context.h"

#include <chrono>
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

namespace fs = std::filesystem;
using namespace vhsdecode::cppport;

namespace {

std::unordered_map<std::string, std::string> read_kv(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open config.kv");
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
std::optional<T> parse_opt_num(const std::unordered_map<std::string, std::string>& kv,
                               const std::string& key) {
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
    if (it == kv.end()) return {};
    return it->second;
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

std::vector<double> read_f64(const fs::path& path) {
    return read_bin<double>(path);
}

K1BuildConfig load_config(const std::unordered_map<std::string, std::string>& kv) {
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
    const int custom_count = parse_num<int>(kv, "video_custom_luma_filters_count");
    for (int i = 0; i < custom_count; ++i) {
        CustomVideoFilterSpec spec{};
        const std::string prefix = "video_custom_luma_filters." + std::to_string(i) + ".";
        const std::string type = parse_str(kv, prefix + "type");
        if (type == "file") {
            spec.type = CustomVideoFilterType::File;
            spec.filename = parse_str(kv, prefix + "filename");
        } else if (type == "highshelf") {
            spec.type = CustomVideoFilterType::HighShelf;
            spec.midfreq = parse_num<double>(kv, prefix + "midfreq");
            spec.gain = parse_num<double>(kv, prefix + "gain");
            spec.q = parse_num<double>(kv, prefix + "q");
        } else if (type == "lowshelf") {
            spec.type = CustomVideoFilterType::LowShelf;
            spec.midfreq = parse_num<double>(kv, prefix + "midfreq");
            spec.gain = parse_num<double>(kv, prefix + "gain");
            spec.q = parse_num<double>(kv, prefix + "q");
        } else {
            throw std::runtime_error("unsupported custom filter type in bench config: " + type);
        }
        dp.video_custom_luma_filters.push_back(spec);
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

bool read_u8_block(std::ifstream& in, std::uint64_t sample, std::size_t blocklen, std::vector<double>& out) {
    out.resize(blocklen);
    in.clear();
    in.seekg(static_cast<std::streamoff>(sample), std::ios::beg);
    if (!in.good()) return false;
    std::vector<std::uint8_t> tmp(blocklen);
    in.read(reinterpret_cast<char*>(tmp.data()), static_cast<std::streamsize>(blocklen));
    if (in.gcount() != static_cast<std::streamsize>(blocklen)) return false;
    for (std::size_t i = 0; i < blocklen; ++i) out[i] = static_cast<double>(tmp[i]);
    return true;
}

double rms_diff(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) throw std::runtime_error("size mismatch");
    long double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const long double d = static_cast<long double>(a[i]) - static_cast<long double>(b[i]);
        sum += d * d;
    }
    return std::sqrt(static_cast<double>(sum / static_cast<long double>(a.size())));
}

double signal_rms(const std::vector<double>& a) {
    long double sum = 0.0;
    for (double v : a) sum += static_cast<long double>(v) * static_cast<long double>(v);
    return std::sqrt(static_cast<double>(sum / static_cast<long double>(a.size())));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: vhsdecode_k1_file_stream <raw_u8_file> <k1_parity_dir> <iterations> [stride_samples]\n";
        return 2;
    }

    const fs::path raw_path = argv[1];
    const fs::path fixture_dir = argv[2];
    const int iterations = std::stoi(argv[3]);
    const auto kv = read_kv(fixture_dir / "config.kv");
    const auto cfg = load_config(kv);
    const std::uint64_t start_sample = parse_num<std::uint64_t>(kv, "sample");
    const std::uint64_t blockcut = parse_num<std::uint64_t>(kv, "blockcut");
    const std::uint64_t blockcut_end = parse_num<std::uint64_t>(kv, "blockcut_end");
    const std::uint64_t stride = (argc >= 5)
        ? static_cast<std::uint64_t>(std::stoull(argv[4]))
        : static_cast<std::uint64_t>(cfg.blocklen) - (blockcut + blockcut_end);

    std::ifstream in(raw_path, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "failed to open raw file: " << raw_path << "\n";
        return 1;
    }

    auto ctx = build_k1_context(cfg);
    const auto pristine_state = ctx.state;
    std::vector<double> block;

    if (!read_u8_block(in, start_sample, cfg.blocklen, block)) {
        std::cerr << "failed to read initial block\n";
        return 1;
    }

    DemodBlockInput input{};
    input.data = block;
    auto first = demodblock(input, ctx.filters, ctx.options, cfg.freq_hz, &ctx.state);

    const auto py_env = read_f64(fixture_dir / "python_envelope.f64");
    const auto py_demod = read_f64(fixture_dir / "python_demod.f64");
    const auto py_demod_05 = read_f64(fixture_dir / "python_demod_05.f64");
    const auto py_burst = read_f64(fixture_dir / "python_demod_burst.f64");

    const double env_rms = rms_diff(first.env, py_env);
    const double demod_rms = rms_diff(first.out_video, py_demod);
    const double demod05_rms = rms_diff(first.out_video05, py_demod_05);
    const double burst_rms = rms_diff(first.out_chroma, py_burst);

    ctx.state = pristine_state;
    std::uint64_t sample = start_sample;
    int completed = 0;
    volatile double sink = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (!read_u8_block(in, sample, cfg.blocklen, block)) break;
        input.data = block;
        auto out = demodblock(input, ctx.filters, ctx.options, cfg.freq_hz, &ctx.state);
        sink += out.out_video.empty() ? 0.0 : out.out_video[0];
        sample += stride;
        ++completed;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const std::chrono::duration<double> dt = t1 - t0;
    const double fps = completed / dt.count();
    const double ms_per_block = (dt.count() * 1000.0) / static_cast<double>(std::max(completed, 1));
    const double rel_env = (env_rms / signal_rms(py_env)) * 100.0;
    const double rel_demod = (demod_rms / signal_rms(py_demod)) * 100.0;
    const double rel_demod05 = (demod05_rms / signal_rms(py_demod_05)) * 100.0;
    const double rel_burst = (burst_rms / signal_rms(py_burst)) * 100.0;

    std::cout << std::fixed << std::setprecision(10);
    std::cout << "blocks=" << completed << "\n";
    std::cout << "fps=" << fps << "\n";
    std::cout << "ms_per_block=" << ms_per_block << "\n";
    std::cout << "stride_samples=" << stride << "\n";
    std::cout << "k1_env_rms=" << env_rms << "\n";
    std::cout << "k1_env_rms_pct_signal=" << rel_env << "\n";
    std::cout << "k1_demod_rms=" << demod_rms << "\n";
    std::cout << "k1_demod_rms_pct_signal=" << rel_demod << "\n";
    std::cout << "k1_demod05_rms=" << demod05_rms << "\n";
    std::cout << "k1_demod05_rms_pct_signal=" << rel_demod05 << "\n";
    std::cout << "k1_burst_rms=" << burst_rms << "\n";
    std::cout << "k1_burst_rms_pct_signal=" << rel_burst << "\n";
    std::cout << "sink=" << sink << "\n";
    return 0;
}

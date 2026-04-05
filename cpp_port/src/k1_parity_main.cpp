#include "vhsdecode_cpp/k1_context.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <chrono>
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
    if (!in.is_open()) {
        throw std::runtime_error("failed to open config.kv");
    }
    std::unordered_map<std::string, std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        out.emplace(line.substr(0, pos), line.substr(pos + 1));
    }
    return out;
}

template <typename T>
T parse_num(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) {
        throw std::runtime_error("missing numeric key: " + key);
    }
    std::istringstream iss(it->second);
    T value{};
    iss >> value;
    return value;
}

template <typename T>
std::optional<T> parse_opt_num(const std::unordered_map<std::string, std::string>& kv,
                               const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return std::nullopt;
    std::istringstream iss(it->second);
    T value{};
    iss >> value;
    return value;
}

bool parse_bool(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end()) return false;
    return it->second == "1" || it->second == "true" || it->second == "True";
}

std::string parse_str(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end()) return {};
    return it->second;
}

std::vector<double> read_i16_as_double(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("failed to open raw_i16.bin");
    in.seekg(0, std::ios::end);
    std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if ((size % static_cast<std::streamsize>(sizeof(std::int16_t))) != 0) {
        throw std::runtime_error("raw_i16.bin has invalid size");
    }
    std::vector<std::int16_t> tmp(static_cast<std::size_t>(size / sizeof(std::int16_t)));
    in.read(reinterpret_cast<char*>(tmp.data()), size);
    std::vector<double> out(tmp.size());
    for (std::size_t i = 0; i < tmp.size(); ++i) out[i] = static_cast<double>(tmp[i]);
    return out;
}

std::vector<double> read_u8_as_double(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("failed to open raw_u8.bin");
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> tmp(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(tmp.data()), size);
    std::vector<double> out(tmp.size());
    for (std::size_t i = 0; i < tmp.size(); ++i) out[i] = static_cast<double>(tmp[i]);
    return out;
}

void write_f64(const fs::path& path, const std::vector<double>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) throw std::runtime_error("failed to open output file");
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(double)));
}

void write_c128(const fs::path& path, const std::vector<std::complex<double>>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) throw std::runtime_error("failed to open complex output file");
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(std::complex<double>)));
}

void write_sos_flat(const fs::path& path, const SosFilter& filt) {
    std::vector<double> flat;
    flat.reserve(filt.sections.size() * 6U);
    for (const auto& sec : filt.sections) {
        flat.push_back(sec.b0);
        flat.push_back(sec.b1);
        flat.push_back(sec.b2);
        flat.push_back(sec.a0);
        flat.push_back(sec.a1);
        flat.push_back(sec.a2);
    }
    write_f64(path, flat);
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
        // DIVERGENCE: K1BuildConfig stores `tape_format` as `const char*`.
        // The parity runner keeps a static-owned copy for the lifetime of the
        // process instead of mirroring Python's native string object.
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
            throw std::runtime_error("unsupported custom filter type in parity config: " + type);
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

}  // namespace

int main(int argc, char** argv) {
    bool no_write = false;
    int bench_iters = 0;
    fs::path dir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--no-write") {
            no_write = true;
        } else if (arg == "--bench") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --bench\n";
                return 2;
            }
            bench_iters = std::stoi(argv[++i]);
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option: " << arg << "\n";
            return 2;
        } else if (dir.empty()) {
            dir = arg;
        } else {
            std::cerr << "usage: vhsdecode_k1_parity <parity_dir> [--no-write] [--bench N]\n";
            return 2;
        }
    }
    if (dir.empty()) {
        std::cerr << "usage: vhsdecode_k1_parity <parity_dir> [--no-write] [--bench N]\n";
        return 2;
    }
    const auto kv = read_kv(dir / "config.kv");
    const auto cfg = load_config(kv);
    auto ctx = build_k1_context(cfg);
    DemodBlockInput input{};
    if (fs::exists(dir / "raw_u8.bin")) {
        input.data = read_u8_as_double(dir / "raw_u8.bin");
    } else {
        input.data = read_i16_as_double(dir / "raw_i16.bin");
    }
    const auto pristine_state = ctx.state;
    auto write_outputs = [&](const DemodBlockResult& out) {
        write_f64(dir / "cpp_envelope.f64", out.env);
        write_c128(dir / "cpp_hilbert.c128", out.hilbert);
        write_f64(dir / "cpp_demod_raw.f64", out.demod);
        write_f64(dir / "cpp_demod_pre_spike.f64", out.demod_pre_spike);
        write_f64(dir / "cpp_demod_diffed.f64", out.demod_diffed);
        write_f64(dir / "cpp_demod.f64", out.out_video);
        write_f64(dir / "cpp_demod_05.f64", out.out_video05);
        write_f64(dir / "cpp_demod_burst.f64", out.out_chroma);
        write_f64(dir / "cpp_chroma_after_sos.f64", out.chroma_after_sos);
        write_f64(dir / "cpp_chroma_after_audio_notch.f64", out.chroma_after_audio_notch);
        write_f64(dir / "cpp_chroma_after_video_notch.f64", out.chroma_after_video_notch);
        write_f64(dir / "cpp_chroma_final_rebuilt.f64", out.out_chroma);
        write_c128(dir / "cpp_filter_RFVideo.c128", ctx.filters.rf_video);
        write_c128(dir / "cpp_filter_hilbert.c128", ctx.filters.hilbert);
        write_c128(dir / "cpp_filter_FVideo.c128", ctx.filters.fvideo);
        write_c128(dir / "cpp_filter_FVideo05.c128", ctx.filters.fvideo05);
        write_sos_flat(dir / "cpp_filter_FEnvPost.f64", ctx.filters.fenv_post);
        write_sos_flat(dir / "cpp_filter_FVideoBurst.f64", ctx.filters.fvideo_burst);
        write_c128(dir / "cpp_filter_RF_bpf.c128", ctx.debug.rf_bpf.empty() ? std::vector<std::complex<double>>(ctx.filters.rf_video.size(), {1.0, 0.0}) : ctx.debug.rf_bpf);
        write_c128(dir / "cpp_filter_RF_lpf_extra.c128", ctx.debug.rf_extra_lpf.empty() ? std::vector<std::complex<double>>(ctx.filters.rf_video.size(), {1.0, 0.0}) : ctx.debug.rf_extra_lpf);
        write_c128(dir / "cpp_filter_RF_hpf_extra.c128", ctx.debug.rf_extra_hpf.empty() ? std::vector<std::complex<double>>(ctx.filters.rf_video.size(), {1.0, 0.0}) : ctx.debug.rf_extra_hpf);
        write_c128(dir / "cpp_filter_RF_peak.c128", ctx.debug.rf_peak.empty() ? std::vector<std::complex<double>>(ctx.filters.rf_video.size(), {1.0, 0.0}) : ctx.debug.rf_peak);
        write_c128(dir / "cpp_filter_RF_audio.c128", ctx.debug.rf_audio_notch.empty() ? std::vector<std::complex<double>>(ctx.filters.rf_video.size(), {1.0, 0.0}) : ctx.debug.rf_audio_notch);
        write_c128(dir / "cpp_filter_RF_ramp.c128", ctx.debug.rf_ramp.empty() ? std::vector<std::complex<double>>(ctx.filters.rf_video.size(), {1.0, 0.0}) : ctx.debug.rf_ramp);
        write_c128(dir / "cpp_filter_deemp.c128", ctx.debug.filter_deemp);
        write_c128(dir / "cpp_filter_video_lpf.c128", ctx.debug.filter_video_lpf);
        write_c128(dir / "cpp_filter_custom.c128", ctx.debug.filter_custom);
        write_c128(dir / "cpp_filter_05.c128", ctx.debug.filter_05);
        write_f64(dir / "cpp_filter_05_taps.f64", ctx.debug.filter_05_taps);
    };
    auto run_once = [&]() {
        auto state = pristine_state;
        return demodblock(input, ctx.filters, ctx.options, cfg.freq_hz, &state);
    };

    if (bench_iters > 0) {
        using clock = std::chrono::steady_clock;
        double total_s = 0.0;
        for (int i = 0; i < bench_iters; ++i) {
            const auto t0 = clock::now();
            auto out = run_once();
            const auto t1 = clock::now();
            total_s += std::chrono::duration<double>(t1 - t0).count();
            if (!no_write && i == bench_iters - 1) write_outputs(out);
        }
        const double avg_s = total_s / static_cast<double>(bench_iters);
        std::cout << "{\n"
                  << "  \"bench_iterations\": " << bench_iters << ",\n"
                  << "  \"avg_compute_s\": " << avg_s << ",\n"
                  << "  \"throughput_per_s\": " << (avg_s > 0.0 ? 1.0 / avg_s : 0.0) << "\n"
                  << "}\n";
        return 0;
    }

    auto out = run_once();
    if (!no_write) write_outputs(out);
    return 0;
}

#include "vhsdecode_cpp/chroma_process.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::unordered_map<std::string, std::string> read_kv(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open config kv");
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
    if (it == kv.end() || it->second.empty()) throw std::runtime_error("missing key: " + key);
    std::istringstream iss(it->second);
    T value{};
    iss >> value;
    return value;
}

template <typename T>
T parse_num_default(const std::unordered_map<std::string, std::string>& kv,
                    const std::string& key,
                    T default_value) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return default_value;
    std::istringstream iss(it->second);
    T value{};
    iss >> value;
    return value;
}

std::vector<double> read_f64(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("failed to open f64");
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if ((size % static_cast<std::streamsize>(sizeof(double))) != 0) throw std::runtime_error("bad f64 size");
    std::vector<double> out(static_cast<std::size_t>(size / sizeof(double)));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return out;
}

std::vector<std::uint16_t> read_u16(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("failed to open u16");
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if ((size % static_cast<std::streamsize>(sizeof(std::uint16_t))) != 0) throw std::runtime_error("bad u16 size");
    std::vector<std::uint16_t> out(static_cast<std::size_t>(size / sizeof(std::uint16_t)));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return out;
}

std::optional<vhsdecode::cppport::IirFilter> read_ba_optional(const fs::path& path) {
    if (!fs::exists(path)) return std::nullopt;
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open ba");
    vhsdecode::cppport::IirFilter filt;
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

void write_f64(const fs::path& path, const std::vector<double>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(double)));
}

void write_u16(const fs::path& path, const std::vector<std::uint16_t>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(std::uint16_t)));
}

std::vector<std::vector<double>> read_phase_matrix(const fs::path& path) {
    auto flat = read_f64(path);
    std::size_t len = flat.size() / 4U;
    std::vector<std::vector<double>> out(4, std::vector<double>(len));
    for (std::size_t p = 0; p < 4; ++p) {
        std::copy(flat.begin() + static_cast<std::ptrdiff_t>(p * len),
                  flat.begin() + static_cast<std::ptrdiff_t>((p + 1) * len),
                  out[p].begin());
    }
    return out;
}

vhsdecode::cppport::SosFilter read_sos_flat(const fs::path& path) {
    auto flat = read_f64(path);
    vhsdecode::cppport::SosFilter sos;
    for (std::size_t i = 0; i < flat.size(); i += 6) {
        sos.sections.push_back({flat[i+0], flat[i+1], flat[i+2], flat[i+3], flat[i+4], flat[i+5]});
    }
    return sos;
}

std::vector<vhsdecode_cpp::PhaseSequenceEntry> read_phase_sequence_csv(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open phase sequence csv");
    std::vector<vhsdecode_cpp::PhaseSequenceEntry> out;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) { first = false; continue; }
        if (line.empty()) continue;
        std::istringstream iss(line);
        std::string tok;
        vhsdecode_cpp::PhaseSequenceEntry e{};
        std::getline(iss, tok, ','); e.line_number = std::stoi(tok);
        std::getline(iss, tok, ','); e.current_phase = std::stoi(tok);
        std::getline(iss, tok, ','); e.burst_phase = std::stod(tok);
        std::getline(iss, tok, ','); e.burst_magnitude = std::stod(tok);
        std::getline(iss, tok, ','); e.burst_i = std::stod(tok);
        std::getline(iss, tok, ','); e.burst_q = std::stod(tok);
        out.push_back(e);
    }
    return out;
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
            std::cerr << "usage: vhsdecode_k3_process_parity <parity_dir> [--no-write] [--bench N]\n";
            return 2;
        }
    }
    if (dir.empty()) {
        std::cerr << "usage: vhsdecode_k3_process_parity <parity_dir> [--no-write] [--bench N]\n";
        return 2;
    }
    const auto kv = read_kv(dir / "process_config.kv");

    vhsdecode_cpp::ChromaProcessInput input{};
    input.chroma = read_f64(dir / "chroma_downscaled.f64");
    input.chroma_heterodyne = read_phase_matrix(dir / "chroma_heterodyne_flat.f64");
    input.phase_sequence = read_phase_sequence_csv(dir / "python_phase_sequence.csv");
    input.fchroma_final = read_sos_flat(dir / "fchroma_final_sos.f64");
    if (fs::exists(dir / "cafc_bandpass_sos.f64")) input.cafc_chroma_bandpass = read_sos_flat(dir / "cafc_bandpass_sos.f64");
    input.fvideo_notch = read_ba_optional(dir / "fvideo_notch.ba");
    input.chroma_audio_notch = read_ba_optional(dir / "chroma_audio_notch.ba");
    input.chroma_deemphasis = read_ba_optional(dir / "chroma_deemphasis.ba");
    input.lineoffset = parse_num<int>(kv, "lineoffset");
    input.linesout = parse_num<int>(kv, "linesout");
    input.outwidth = parse_num<int>(kv, "outwidth");
    input.burst_start = parse_num<int>(kv, "burst_start");
    input.burst_end = parse_num<int>(kv, "burst_end");
    input.cafc_move = parse_num_default<int>(kv, "cafc_move", 3);
    input.is_ntsc = parse_num<int>(kv, "is_ntsc") != 0;
    input.do_cafc = parse_num_default<int>(kv, "do_cafc", 0) != 0;
    input.disable_deemph = parse_num_default<int>(kv, "disable_deemph", 0) != 0;
    input.disable_comb = parse_num_default<int>(kv, "disable_comb", 0) != 0;
    input.disable_tracking_cafc = parse_num_default<int>(kv, "disable_tracking_cafc", 0) != 0;
    input.disable_phase_correction = parse_num_default<int>(kv, "disable_phase_correction", 0) != 0;
    input.do_chroma_deemphasis = parse_num_default<int>(kv, "do_chroma_deemphasis", 0) != 0;
    input.enable_video_notch = parse_num_default<int>(kv, "enable_video_notch", 0) != 0;
    if (parse_num<int>(kv, "burst_phase_avg_present") != 0) {
        input.burst_phase_avg = parse_num<double>(kv, "burst_phase_avg");
    }
    input.burst_abs_ref = parse_num<double>(kv, "burst_abs_ref");

    auto write_outputs = [&](const vhsdecode_cpp::ChromaProcessResult& result) {
        write_f64(dir / "cpp_chroma_after_cafc_prefilter.f64", result.chroma_after_cafc_prefilter);
        write_f64(dir / "cpp_chroma_after_burst_deemph.f64", result.chroma_after_burst_deemph);
        write_f64(dir / "cpp_uphet_raw_process.f64", result.uphet_raw);
        write_f64(dir / "cpp_uphet_phase_comp.f64", result.uphet_phase_comp);
        write_f64(dir / "cpp_uphet_filtered.f64", result.uphet_filtered);
        write_f64(dir / "cpp_uphet_after_chroma_deemph.f64", result.uphet_after_chroma_deemph);
        write_f64(dir / "cpp_uphet_comb.f64", result.uphet_comb);
        write_f64(dir / "cpp_uphet_final.f64", result.uphet_final);
        write_u16(dir / "cpp_chroma_u16.u16", result.chroma_u16);
        std::ofstream out(dir / "cpp_process_result.kv");
        out << "mean_rms=" << result.mean_rms << "\n";
        if (result.cafc_measurement.has_value()) {
            out << "cafc_spec_hz=" << result.cafc_measurement->spec_hz << "\n";
            out << "cafc_measured_hz=" << result.cafc_measurement->measured_hz << "\n";
            out << "cafc_long_term_offset_hz=" << result.cafc_measurement->long_term_offset_hz << "\n";
            out << "cafc_cc_phase_rad=" << result.cafc_measurement->cc_phase_rad << "\n";
        }
    };
    auto run_once = [&]() {
        std::optional<vhsdecode::cppport::ChromaAfc> cafc;
        input.chroma_afc = nullptr;
        if (input.do_cafc) {
            vhsdecode::cppport::ChromaAfcConfig cfg{};
            cfg.demod_rate_hz = parse_num<double>(kv, "cafc_demod_rate_hz");
            cfg.under_ratio = parse_num<double>(kv, "cafc_under_ratio");
            cfg.fps = parse_num<double>(kv, "cafc_fps");
            cfg.frame_lines = parse_num<int>(kv, "cafc_frame_lines");
            cfg.max_field_lines = parse_num<int>(kv, "cafc_max_field_lines");
            cfg.outlinelen = parse_num<int>(kv, "cafc_outlinelen");
            cfg.fsc_mhz = parse_num<double>(kv, "cafc_fsc_mhz");
            cfg.color_under_carrier_hz = parse_num<double>(kv, "cafc_color_under_carrier_hz");
            cfg.chroma_bandpass_order = parse_num<int>(kv, "cafc_chroma_bandpass_order");
            cfg.linearize = parse_num<int>(kv, "cafc_linearize") != 0;
            cfg.do_cafc = true;
            cfg.chroma_bpf_lower_hz = parse_num<double>(kv, "cafc_chroma_bpf_lower_hz");
            cafc.emplace(cfg);
            cafc->setCC(parse_num<double>(kv, "cafc_cc_hz"));
            cafc->setCCPhase(parse_num<double>(kv, "cafc_cc_phase_rad"));
            input.chroma_afc = &*cafc;
        }
        return vhsdecode_cpp::process_chroma(input);
    };

    if (bench_iters > 0) {
        using clock = std::chrono::steady_clock;
        double total_s = 0.0;
        for (int i = 0; i < bench_iters; ++i) {
            const auto t0 = clock::now();
            auto result = run_once();
            const auto t1 = clock::now();
            total_s += std::chrono::duration<double>(t1 - t0).count();
            if (!no_write && i == bench_iters - 1) write_outputs(result);
        }
        const double avg_s = total_s / static_cast<double>(bench_iters);
        std::cout << "{\n"
                  << "  \"bench_iterations\": " << bench_iters << ",\n"
                  << "  \"avg_compute_s\": " << avg_s << ",\n"
                  << "  \"throughput_per_s\": " << (avg_s > 0.0 ? 1.0 / avg_s : 0.0) << "\n"
                  << "}\n";
        return 0;
    }

    const auto result = run_once();
    if (!no_write) write_outputs(result);
    return 0;
}

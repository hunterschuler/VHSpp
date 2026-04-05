#include "vhsdecode_cpp/sync_core.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cmath>
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
    if (!in.is_open()) {
        throw std::runtime_error("failed to open kv file");
    }
    std::unordered_map<std::string, std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        out.emplace(line.substr(0, pos), line.substr(pos + 1));
    }
    return out;
}

template <typename T>
T parse_num(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    const auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) {
        throw std::runtime_error("missing numeric key: " + key);
    }
    std::istringstream iss(it->second);
    T value{};
    iss >> value;
    return value;
}

std::vector<double> read_f64(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("failed to open f64 input");
    }
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size % static_cast<std::streamsize>(sizeof(double)) != 0) {
        throw std::runtime_error("bad f64 file size");
    }
    std::vector<double> out(static_cast<std::size_t>(size / sizeof(double)));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return out;
}

std::vector<int> read_i32(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("failed to open i32 input");
    }
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size % static_cast<std::streamsize>(sizeof(std::int32_t)) != 0) {
        throw std::runtime_error("bad i32 file size");
    }
    std::vector<std::int32_t> tmp(static_cast<std::size_t>(size / sizeof(std::int32_t)));
    in.read(reinterpret_cast<char*>(tmp.data()), size);
    std::vector<int> out(tmp.begin(), tmp.end());
    return out;
}

std::vector<vhsdecode_cpp::ValidPulse> read_valid_pulses(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        throw std::runtime_error("failed to open valid pulses csv");
    }
    std::vector<vhsdecode_cpp::ValidPulse> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line == "type,start,valid") {
            continue;
        }
        std::istringstream iss(line);
        std::string tok;
        vhsdecode_cpp::ValidPulse vp{};
        std::getline(iss, tok, ',');
        vp.type = std::stoi(tok);
        std::getline(iss, tok, ',');
        vp.start = std::stoi(tok);
        std::getline(iss, tok, ',');
        if (tok == "True" || tok == "true") {
            vp.valid = 1;
        } else if (tok == "False" || tok == "false") {
            vp.valid = 0;
        } else {
            vp.valid = std::stoi(tok);
        }
        out.push_back(vp);
    }
    return out;
}

void write_f64(const fs::path& path, const std::vector<double>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(double)));
}

void write_u8(const fs::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(std::uint8_t)));
}

void write_json_result(const fs::path& path, const vhsdecode_cpp::FirstHsyncResult& r) {
    std::ofstream out(path);
    out << std::setprecision(17);
    out << "{\n";
    out << "  \"has_line0loc\": " << (r.has_line0loc ? "true" : "false") << ",\n";
    out << "  \"line0loc\": " << r.line0loc << ",\n";
    out << "  \"has_first_hsync_loc\": " << (r.has_first_hsync_loc ? "true" : "false") << ",\n";
    out << "  \"first_hsync_loc\": " << r.first_hsync_loc << ",\n";
    out << "  \"hsync_start_line\": " << r.hsync_start_line << ",\n";
    out << "  \"has_next_field\": " << (r.has_next_field ? "true" : "false") << ",\n";
    out << "  \"next_field\": " << r.next_field << ",\n";
    out << "  \"first_field\": " << (r.first_field ? "true" : "false") << ",\n";
    out << "  \"progressive_field\": " << (r.progressive_field ? "true" : "false") << ",\n";
    out << "  \"prev_hsync_diff\": " << r.prev_hsync_diff << ",\n";
    out << "  \"vblank_pulses\": [";
    for (std::size_t i = 0; i < r.vblank_pulses.size(); ++i) {
        if (i) out << ", ";
        out << r.vblank_pulses[i];
    }
    out << "]\n}\n";
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
            std::cerr << "usage: vhsdecode_k2_sync_parity <parity_dir> [--no-write] [--bench N]\n";
            return 2;
        }
    }
    if (dir.empty()) {
        std::cerr << "usage: vhsdecode_k2_sync_parity <parity_dir> [--no-write] [--bench N]\n";
        return 2;
    }
    const auto kv = read_kv(dir / "sync_config.kv");

    vhsdecode_cpp::FirstHsyncInput first_input{};
    first_input.validpulses = read_valid_pulses(dir / "validpulses.csv");
    first_input.meanlinelen = parse_num<double>(kv, "meanlinelen");
    first_input.is_ntsc = parse_num<int>(kv, "is_ntsc") != 0;
    first_input.field_lines = read_i32(dir / "field_lines.i32");
    first_input.num_eq_pulses = parse_num<int>(kv, "num_eq_pulses");
    first_input.prev_first_field = parse_num<int>(kv, "prev_first_field");
    first_input.last_field_offset_lines = parse_num<double>(kv, "last_field_offset_lines");
    first_input.prev_first_hsync_loc = parse_num<double>(kv, "prev_first_hsync_loc");
    first_input.prev_hsync_diff = parse_num<double>(kv, "prev_hsync_diff");
    first_input.field_order_confidence = parse_num<int>(kv, "field_order_confidence");
    first_input.fallback_line0loc = parse_num<double>(kv, "fallback_line0loc");
    first_input.fallback_is_first_field = parse_num<int>(kv, "fallback_is_first_field");
    first_input.fallback_is_first_field_confidence =
        parse_num<int>(kv, "fallback_is_first_field_confidence");

    vhsdecode_cpp::HsyncRefineInput refine_input{};
    refine_input.linelocs1 = read_f64(dir / "linelocs1.f64");
    refine_input.demod_05 = read_f64(dir / "demod_05.f64");
    refine_input.normal_hsync_length = parse_num<int>(kv, "normal_hsync_length");
    refine_input.one_usec = parse_num<int>(kv, "one_usec");
    refine_input.sample_rate_mhz = parse_num<double>(kv, "sample_rate_mhz");
    refine_input.is_pal = parse_num<int>(kv, "is_pal") != 0;
    refine_input.disable_right_hsync = parse_num<int>(kv, "disable_right_hsync") != 0;
    refine_input.hsync_threshold = parse_num<double>(kv, "hsync_threshold");
    refine_input.ire_30 = parse_num<double>(kv, "ire_30");
    refine_input.ire_n_65 = parse_num<double>(kv, "ire_n_65");
    refine_input.ire_110 = parse_num<double>(kv, "ire_110");
    auto linebad_i32 = read_i32(dir / "linebad.i32");

    struct RunResult {
        vhsdecode_cpp::FirstHsyncResult first_result;
        vhsdecode_cpp::ValidPulsesToLinelocsResult lineloc_result;
        std::vector<double> linelocs2;
        std::vector<std::uint8_t> linebad;
    };

    auto run_once = [&]() {
        RunResult out{};
        out.first_result = vhsdecode_cpp::get_first_hsync_loc(first_input);
        std::vector<double> pulse_starts;
        pulse_starts.reserve(first_input.validpulses.size());
        for (const auto& p : first_input.validpulses) pulse_starts.push_back(static_cast<double>(p.start));
        out.lineloc_result = vhsdecode_cpp::valid_pulses_to_linelocs(
            pulse_starts,
            static_cast<int>(std::lround(out.first_result.first_hsync_loc)),
            static_cast<int>(std::lround(out.first_result.hsync_start_line)),
            first_input.meanlinelen,
            parse_num<double>(kv, "hsync_tolerance"),
            parse_num<int>(kv, "proclines"),
            parse_num<double>(kv, "gap_detection_threshold"));
        out.linebad.assign(linebad_i32.begin(), linebad_i32.end());
        out.linelocs2 = vhsdecode_cpp::refine_linelocs_hsync(refine_input, out.linebad);
        return out;
    };

    if (bench_iters > 0) {
        using clock = std::chrono::steady_clock;
        double total_s = 0.0;
        for (int i = 0; i < bench_iters; ++i) {
            const auto t0 = clock::now();
            auto out = run_once();
            const auto t1 = clock::now();
            total_s += std::chrono::duration<double>(t1 - t0).count();
            if (!no_write && i == bench_iters - 1) {
                write_json_result(dir / "cpp_first_hsync.json", out.first_result);
                write_f64(dir / "cpp_linelocs0.f64", out.lineloc_result.line_locations);
                write_u8(dir / "cpp_lineloc_errs.u8", out.lineloc_result.line_location_errs);
                write_f64(dir / "cpp_linelocs2.f64", out.linelocs2);
                write_u8(dir / "cpp_linebad.u8", out.linebad);
            }
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
    if (!no_write) {
        write_json_result(dir / "cpp_first_hsync.json", out.first_result);
        write_f64(dir / "cpp_linelocs0.f64", out.lineloc_result.line_locations);
        write_u8(dir / "cpp_lineloc_errs.u8", out.lineloc_result.line_location_errs);
        write_f64(dir / "cpp_linelocs2.f64", out.linelocs2);
        write_u8(dir / "cpp_linebad.u8", out.linebad);
    }

    return 0;
}

#include "vhsdecode_cpp/resync_runtime.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace vhsdecode_cpp;

namespace {

std::unordered_map<std::string, std::string> read_kv(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open kv file");
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
    if (!in.is_open()) throw std::runtime_error("failed to open f64 input");
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<double> out(static_cast<std::size_t>(size / sizeof(double)));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return out;
}

void write_f64(const fs::path& path, const std::vector<double>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(double)));
}

void write_pulses(const fs::path& path, const std::vector<Pulse>& pulses) {
    std::ofstream out(path);
    out << "start,len\n";
    for (const auto& p : pulses) {
        out << p.start << "," << p.len << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: vhsdecode_k2_get_pulses_parity <parity_dir>\n";
        return 2;
    }

    const fs::path dir = argv[1];
    const auto kv = read_kv(dir / "get_pulses_config.kv");

    ResyncRuntimeConfig cfg{};
    cfg.sample_rate_hz = parse_num<double>(kv, "sample_rate_hz");
    cfg.sample_rate_mhz = parse_num<double>(kv, "sample_rate_mhz");
    cfg.divisor = parse_num<int>(kv, "divisor");
    cfg.fps = parse_num<double>(kv, "fps");
    cfg.frame_lines = parse_num<int>(kv, "frame_lines");
    cfg.eq_pulse_us = parse_num<double>(kv, "eq_pulse_us");
    cfg.vsync_pulse_us = parse_num<double>(kv, "vsync_pulse_us");
    cfg.sysparams_const.ire0 = parse_num<double>(kv, "ire0");
    cfg.sysparams_const.hz_ire = parse_num<double>(kv, "hz_ire");
    cfg.sysparams_const.vsync_hz = parse_num<double>(kv, "vsync_hz");

    ResyncFieldInput input{};
    input.demod = read_f64(dir / "python_demod.f64");
    input.demod_05 = read_f64(dir / "python_demod_05.f64");
    input.color_system = parse_num<int>(kv, "color_system_ntsc_like") != 0 ? "NTSC" : "405";
    input.fallback_vsync = parse_num<int>(kv, "fallback_vsync") != 0;
    input.disable_dc_offset = parse_num<int>(kv, "disable_dc_offset") != 0;

    ResyncRuntime runtime(cfg);
    const auto pulses = runtime.get_pulses(input, true);
    write_pulses(dir / "cpp_pulses.csv", pulses);
    write_f64(dir / "cpp_demod_after_get_pulses.f64", input.demod);
    write_f64(dir / "cpp_demod_05_after_get_pulses.f64", input.demod_05);
    std::ofstream out(dir / "cpp_get_pulses_debug.kv");
    out << "last_pulse_threshold=" << runtime.last_pulse_threshold() << "\n";
    out << "eq_pulselen=" << runtime.eq_pulselen() << "\n";
    out << "linelen=" << runtime.linelen() << "\n";
    out << "long_pulse_max=" << runtime.long_pulse_max() << "\n";
    return 0;
}

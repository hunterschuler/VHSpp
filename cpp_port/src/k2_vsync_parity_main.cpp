#include "vhsdecode_cpp/vsync_serration.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

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

void write_i32(const fs::path& path, const std::vector<int>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(int)));
}

void write_json(const fs::path& path,
                const vhsdecode::cppport::VsyncSerration& serration) {
    std::ofstream out(path);
    out << std::setprecision(17);
    out << "{\n";
    out << "  \"has_serration\": " << (serration.has_serration() ? "true" : "false") << ",\n";
    out << "  \"has_levels\": " << (serration.has_levels() ? "true" : "false");
    const auto levels = serration.pull_levels();
    if (levels.has_value()) {
        out << ",\n  \"sync_level\": " << levels->first
            << ",\n  \"blank_level\": " << levels->second;
    }
    out << ",\n  \"eq_pulselen\": " << serration.get_eq_pulselen()
        << ",\n  \"linelen\": " << serration.get_line_len() << "\n}\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: vhsdecode_k2_vsync_parity <parity_dir>\n";
        return 2;
    }
    const fs::path dir = argv[1];
    const auto kv = read_kv(dir / "vsync_config.kv");
    const auto demod05 = read_f64(dir / "python_demod_05.f64");

    vhsdecode::cppport::VsyncSerrationConfig cfg{};
    cfg.sample_rate_hz = parse_num<double>(kv, "sample_rate_hz");
    cfg.divisor = parse_num<int>(kv, "divisor");
    cfg.show_decoded_serration = true;
    cfg.sysparams.fps = parse_num<double>(kv, "fps");
    cfg.sysparams.frame_lines = parse_num<int>(kv, "frame_lines");
    cfg.sysparams.eq_pulse_us = parse_num<double>(kv, "eq_pulse_us");

    vhsdecode::cppport::VsyncSerration serration(cfg);
    serration.work(demod05);

    write_json(dir / "cpp_vsync.json", serration);
    write_f64(dir / "cpp_sync_bias.f64", std::vector<double>{serration.sync_level_bias()});
    write_i32(dir / "cpp_minima.i32", serration.last_debug().minima);
    write_i32(dir / "cpp_serrations.i32", serration.last_debug().serrations);
    write_i32(dir / "cpp_arbitrated.i32", serration.last_debug().arbitrated);
    write_i32(dir / "cpp_serration_locs.i32", serration.last_debug().serration_locs);
    write_f64(dir / "cpp_envelope.f64", serration.last_debug().envelope);

    return 0;
}

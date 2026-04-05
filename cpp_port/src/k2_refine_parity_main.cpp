#include "vhsdecode_cpp/sync_core.h"

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
    std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<double> out(static_cast<std::size_t>(size / sizeof(double)));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return out;
}

std::vector<RawPulse> read_raw_pulses(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open raw pulses csv");
    std::vector<RawPulse> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line == "start,len") continue;
        std::istringstream iss(line);
        std::string tok;
        RawPulse p{};
        std::getline(iss, tok, ',');
        p.start = std::stoi(tok);
        std::getline(iss, tok, ',');
        p.len = std::stoi(tok);
        out.push_back(p);
    }
    return out;
}

void write_valid_pulses(const fs::path& path, const std::vector<ValidPulse>& data) {
    std::ofstream out(path);
    out << "type,start,valid\n";
    for (const auto& p : data) {
        out << p.type << "," << p.start << "," << p.valid << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: vhsdecode_k2_refine_parity <parity_dir>\n";
        return 2;
    }
    const fs::path dir = argv[1];
    const auto kv = read_kv(dir / "refine_config.kv");
    auto raw_pulses = read_raw_pulses(dir / "python_rawpulses.csv");
    const auto demod05 = read_f64(dir / "python_demod_05.f64");

    PulseTimingRanges lt{};
    lt.hsync_min = parse_num<double>(kv, "lt_hsync_min");
    lt.hsync_max = parse_num<double>(kv, "lt_hsync_max");
    lt.eq_min = parse_num<double>(kv, "lt_eq_min");
    lt.eq_max = parse_num<double>(kv, "lt_eq_max");
    lt.vsync_min = parse_num<double>(kv, "lt_vsync_min");
    lt.vsync_max = parse_num<double>(kv, "lt_vsync_max");

    const double ire0 = parse_num<double>(kv, "ire0");
    const double hz_ire = parse_num<double>(kv, "hz_ire");
    const double eq_pulselen = parse_num<double>(kv, "eq_pulselen");
    const double long_pulse_max = parse_num<double>(kv, "long_pulse_max");

    // DIVERGENCE: this harness stages K2 parity by reusing Python's raw pulse
    // list and exact timing windows. That isolates the FieldShared.refinepulses()
    // port before widening back out to the full get_pulses()/resync path.
    const auto valid = refine_pulses(
        raw_pulses,
        demod05,
        lt,
        parse_num<int>(kv, "num_pulses"),
        parse_num<int>(kv, "in_line_len"),
        ire0,
        hz_ire,
        eq_pulselen,
        long_pulse_max);

    write_valid_pulses(dir / "cpp_validpulses.csv", valid);
    return 0;
}

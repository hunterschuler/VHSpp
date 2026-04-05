#include "vhsdecode_cpp/resync_core.h"

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

void write_i32(const fs::path& path, const std::vector<int>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(int)));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: vhsdecode_k2_pulse_parity <parity_dir>\n";
        return 2;
    }
    const fs::path dir = argv[1];
    const auto kv = read_kv(dir / "pulse_config.kv");
    const auto demod05 = read_f64(dir / "python_demod_05.f64");

    SysParamsConst sp{};
    sp.ire0 = parse_num<double>(kv, "ire0");
    sp.hz_ire = parse_num<double>(kv, "hz_ire");
    sp.vsync_hz = parse_num<double>(kv, "vsync_hz");

    const auto range = findpulses_range(sp, sp.vsync_hz);
    std::vector<int> starts;
    std::vector<int> lengths;
    findpulses_numba_raw(
        demod05,
        range.pulse_hz_max,
        parse_num<double>(kv, "min_synclen"),
        parse_num<double>(kv, "max_synclen"),
        starts,
        lengths);
    write_i32(dir / "cpp_pulse_starts.i32", starts);
    write_i32(dir / "cpp_pulse_lengths.i32", lengths);

    std::vector<int> rstarts;
    std::vector<int> rlengths;
    findpulses_numba_raw_reduced(
        demod05,
        range.pulse_hz_max,
        parse_num<int>(kv, "divisor"),
        parse_num<double>(kv, "min_synclen"),
        parse_num<double>(kv, "max_synclen"),
        rstarts,
        rlengths);
    write_i32(dir / "cpp_pulse_starts_reduced.i32", rstarts);
    write_i32(dir / "cpp_pulse_lengths_reduced.i32", rlengths);

    std::ofstream out(dir / "cpp_pulse_range.txt");
    out << "pulse_hz_min=" << range.pulse_hz_min << "\n";
    out << "pulse_hz_max=" << range.pulse_hz_max << "\n";
    return 0;
}

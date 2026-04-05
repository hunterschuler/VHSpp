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

std::vector<int> read_i32(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("failed to open i32 input");
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<std::int32_t> tmp(static_cast<std::size_t>(size / sizeof(std::int32_t)));
    in.read(reinterpret_cast<char*>(tmp.data()), size);
    return std::vector<int>(tmp.begin(), tmp.end());
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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: vhsdecode_k2_hsync_refine_parity <case_dir>\n";
        return 2;
    }
    const fs::path dir = argv[1];
    const auto kv = read_kv(dir / "refine_hsync_config.kv");

    vhsdecode_cpp::HsyncRefineInput input{};
    input.linelocs1 = read_f64(dir / "linelocs1.f64");
    input.demod_05 = read_f64(dir / "demod_05.f64");
    input.normal_hsync_length = parse_num<int>(kv, "normal_hsync_length");
    input.one_usec = parse_num<int>(kv, "one_usec");
    input.sample_rate_mhz = parse_num<double>(kv, "sample_rate_mhz");
    input.is_pal = parse_num<int>(kv, "is_pal") != 0;
    input.disable_right_hsync = parse_num<int>(kv, "disable_right_hsync") != 0;
    input.hsync_threshold = parse_num<double>(kv, "hsync_threshold");
    input.ire_30 = parse_num<double>(kv, "ire_30");
    input.ire_n_65 = parse_num<double>(kv, "ire_n_65");
    input.ire_110 = parse_num<double>(kv, "ire_110");

    auto linebad_i32 = read_i32(dir / "linebad.i32");
    std::vector<std::uint8_t> linebad(linebad_i32.begin(), linebad_i32.end());
    const auto linelocs2 = vhsdecode_cpp::refine_linelocs_hsync(input, linebad);
    write_f64(dir / "cpp_linelocs2.f64", linelocs2);
    write_u8(dir / "cpp_linebad.u8", linebad);
    return 0;
}

#include "vhsdecode_cpp/downscale_core.h"

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
    if (!in.is_open()) throw std::runtime_error("failed to open kv");
    std::unordered_map<std::string, std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        out.emplace(line.substr(0, pos), line.substr(pos + 1));
    }
    return out;
}

template <typename T>
T parse_num(const std::unordered_map<std::string, std::string>& kv, const std::string& key) {
    auto it = kv.find(key);
    if (it == kv.end()) throw std::runtime_error("missing key " + key);
    std::istringstream iss(it->second);
    T v{};
    iss >> v;
    return v;
}

template <typename T>
std::vector<T> read_bin(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("failed to open binary");
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size % static_cast<std::streamsize>(sizeof(T)) != 0) throw std::runtime_error("bad binary size");
    std::vector<T> out(static_cast<std::size_t>(size / sizeof(T)));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return out;
}

void write_f32(const fs::path& path, const std::vector<float>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float)));
}

void write_f64(const fs::path& path, const std::vector<double>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(double)));
}

void write_u16(const fs::path& path, const std::vector<std::uint16_t>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(std::uint16_t)));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: vhsdecode_k4_downscale_parity <parity_dir>\n";
        return 2;
    }
    const fs::path dir = argv[1];
    const auto kv = read_kv(dir / "downscale_config.kv");
    vhsdecode_cpp::DownscaleCoreInput input{};
    input.demod = read_bin<float>(dir / "demod.f32");
    input.linelocs = read_bin<double>(dir / "linelocs.f64");
    input.lineoffset = parse_num<int>(kv, "lineoffset");
    input.outlinecount = parse_num<int>(kv, "outlinecount");
    input.outlinelen = parse_num<int>(kv, "outlinelen");
    input.inlinelen = parse_num<double>(kv, "inlinelen");
    input.wow_level_adjust_smoothing = parse_num<double>(kv, "wow_level_adjust_smoothing");
    input.y_comb_limit = parse_num<double>(kv, "y_comb_limit");
    input.final_output = parse_num<int>(kv, "final_output") != 0;
    input.export_raw_tbc = parse_num<int>(kv, "export_raw_tbc") != 0;
    input.ire0 = parse_num<double>(kv, "ire0");
    input.hz_ire = parse_num<double>(kv, "hz_ire");
    input.output_zero = parse_num<double>(kv, "output_zero");
    input.vsync_ire = parse_num<double>(kv, "vsync_ire");
    input.out_scale = parse_num<double>(kv, "out_scale");
    if (const auto it = kv.find("wow_interpolation_method"); it != kv.end()) {
        if (it->second == "quadratic") input.wow_spline_degree = 2;
        else if (it->second == "cubic") input.wow_spline_degree = 3;
        else input.wow_spline_degree = 1;
    }
    const auto result = vhsdecode_cpp::downscale_luma(input);
    write_f32(dir / "cpp_dsout_float.f32", result.dsout_float);
    write_f64(dir / "cpp_interpolated_pixel_locs.f64", result.interpolated_pixel_locs);
    write_f64(dir / "cpp_wowfactors.f64", result.wowfactors);
    if (!result.dsout_u16.empty()) write_u16(dir / "cpp_dsout_u16.u16", result.dsout_u16);
    return 0;
}

#include "vhsdecode_cpp/sync_core.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cmath>
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

std::vector<vhsdecode_cpp::ValidPulse> read_valid_pulses(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open valid pulses csv");
    std::vector<vhsdecode_cpp::ValidPulse> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line == "type,start,valid") continue;
        std::istringstream iss(line);
        std::string tok;
        vhsdecode_cpp::ValidPulse vp{};
        std::getline(iss, tok, ',');
        vp.type = std::stoi(tok);
        std::getline(iss, tok, ',');
        vp.start = std::stoi(tok);
        std::getline(iss, tok, ',');
        vp.valid = std::stoi(tok);
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
    if (argc != 2) {
        std::cerr << "usage: vhsdecode_k2_first_hsync_parity <case_dir>\n";
        return 2;
    }
    const fs::path dir = argv[1];
    const auto kv = read_kv(dir / "first_lineloc_config.kv");

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

    const auto first_result = vhsdecode_cpp::get_first_hsync_loc(first_input);
    write_json_result(dir / "cpp_first_hsync.json", first_result);

    std::vector<double> pulse_starts;
    pulse_starts.reserve(first_input.validpulses.size());
    for (const auto& p : first_input.validpulses) {
        pulse_starts.push_back(static_cast<double>(p.start));
    }

    const auto lineloc_result = vhsdecode_cpp::valid_pulses_to_linelocs(
        pulse_starts,
        static_cast<int>(std::lround(first_result.first_hsync_loc)),
        static_cast<int>(std::lround(first_result.hsync_start_line)),
        first_input.meanlinelen,
        parse_num<double>(kv, "hsync_tolerance"),
        parse_num<int>(kv, "proclines"),
        parse_num<double>(kv, "gap_detection_threshold"));
    write_f64(dir / "cpp_linelocs0.f64", lineloc_result.line_locations);
    write_u8(dir / "cpp_lineloc_errs.u8", lineloc_result.line_location_errs);
    return 0;
}

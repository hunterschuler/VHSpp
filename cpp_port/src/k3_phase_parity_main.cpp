#include "vhsdecode_cpp/chroma_phase.h"

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

std::vector<double> read_f64(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("failed to open f64");
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if ((size % static_cast<std::streamsize>(sizeof(double))) != 0) {
        throw std::runtime_error("bad f64 size");
    }
    std::vector<double> out(static_cast<std::size_t>(size / sizeof(double)));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return out;
}

vhsdecode::cppport::SosFilter read_sos_flat(const fs::path& path) {
    auto flat = read_f64(path);
    if ((flat.size() % 6U) != 0U) throw std::runtime_error("bad sos flat size");
    vhsdecode::cppport::SosFilter sos;
    for (std::size_t i = 0; i < flat.size(); i += 6) {
        sos.sections.push_back({flat[i + 0], flat[i + 1], flat[i + 2], flat[i + 3], flat[i + 4], flat[i + 5]});
    }
    return sos;
}

std::vector<std::vector<double>> read_phase_matrix(const fs::path& path) {
    auto flat = read_f64(path);
    if ((flat.size() % 4U) != 0U) throw std::runtime_error("heterodyne flat size must be divisible by 4");
    std::size_t len = flat.size() / 4U;
    std::vector<std::vector<double>> out(4, std::vector<double>(len));
    for (std::size_t p = 0; p < 4; ++p) {
        std::copy(flat.begin() + static_cast<std::ptrdiff_t>(p * len),
                  flat.begin() + static_cast<std::ptrdiff_t>((p + 1) * len),
                  out[p].begin());
    }
    return out;
}

std::optional<std::vector<int>> read_rotation(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;
    std::vector<int> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        out.push_back(std::stoi(line));
    }
    return out;
}

void write_phase_sequence_csv(const fs::path& path, const std::vector<vhsdecode_cpp::PhaseSequenceEntry>& seq) {
    std::ofstream out(path);
    out << "line_number,current_phase,burst_phase,burst_magnitude,burst_i,burst_q\n";
    out << std::setprecision(17);
    for (const auto& entry : seq) {
        out << entry.line_number << ','
            << entry.current_phase << ','
            << entry.burst_phase << ','
            << entry.burst_magnitude << ','
            << entry.burst_i << ','
            << entry.burst_q << '\n';
    }
}

void write_json(const fs::path& path, const vhsdecode_cpp::ChromaPhaseResult& result) {
    std::ofstream out(path);
    out << std::setprecision(17);
    out << "{\n";
    out << "  \"track_phase\": " << result.track_phase << ",\n";
    out << "  \"burst_phase_avg\": ";
    if (result.burst_phase_avg.has_value()) out << *result.burst_phase_avg; else out << "null";
    out << ",\n  \"burst_detected\": ";
    if (result.burst_detected.has_value()) out << (*result.burst_detected ? "true" : "false"); else out << "null";
    out << "\n}\n";
}

void write_f64(const fs::path& path, const std::vector<double>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size() * sizeof(double)));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: vhsdecode_k3_phase_parity <parity_dir>\n";
        return 2;
    }
    const fs::path dir = argv[1];
    const auto kv = read_kv(dir / "phase_config.kv");

    vhsdecode_cpp::ChromaPhaseInput input{};
    input.chroma = read_f64(dir / "chroma_downscaled.f64");
    input.chroma_heterodyne = read_phase_matrix(dir / "chroma_heterodyne_flat.f64");
    input.chroma_filter = read_sos_flat(dir / "fchroma_final_sos.f64");
    input.chroma_rotation = read_rotation(dir / "chroma_rotation.txt");
    if (parse_num<int>(kv, "track_phase_present") != 0) {
        input.chroma_rotation_index = parse_num<int>(kv, "track_phase");
    }
    input.lineoffset = parse_num<int>(kv, "lineoffset");
    input.linesout = parse_num<int>(kv, "linesout");
    input.outwidth = parse_num<int>(kv, "outwidth");
    input.burst_start = parse_num<int>(kv, "burst_start");
    input.burst_end = parse_num<int>(kv, "burst_end");
    input.burst_sin = read_f64(dir / "fsc_wave.f64");
    input.burst_cos = read_f64(dir / "fsc_cos_wave.f64");
    input.detect_chroma_track_phase = parse_num<int>(kv, "detect_chroma_track_phase") != 0;
    input.rotation_check_start_line = parse_num<int>(kv, "rotation_check_start_line");
    input.is_ntsc = parse_num<int>(kv, "is_ntsc") != 0;

    const auto result = vhsdecode_cpp::get_phase_rotation_sequence(input);
    write_json(dir / "cpp_phase_result.json", result);
    write_phase_sequence_csv(dir / "cpp_phase_sequence.csv", result.phase_sequence);
    write_f64(dir / "cpp_uphet_raw.f64",
              vhsdecode_cpp::upconvert_chroma(input.chroma,
                                              input.lineoffset,
                                              input.outwidth,
                                              input.chroma_heterodyne,
                                              result.phase_sequence));
    return 0;
}

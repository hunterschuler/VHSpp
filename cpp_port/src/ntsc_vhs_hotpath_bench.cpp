#include "vhsdecode_cpp/chroma_phase.h"
#include "vhsdecode_cpp/downscale_core.h"
#include "vhsdecode_cpp/sync_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::unordered_map<std::string, std::string> read_kv(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open kv file: " + path.string());
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

template <typename T>
std::vector<T> read_bin(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("failed to open binary file: " + path.string());
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    if ((size % static_cast<std::streamsize>(sizeof(T))) != 0) {
        throw std::runtime_error("bad binary size for: " + path.string());
    }
    std::vector<T> out(static_cast<std::size_t>(size / sizeof(T)));
    in.read(reinterpret_cast<char*>(out.data()), size);
    return out;
}

std::optional<vhsdecode::cppport::IirFilter> read_ba_optional(const fs::path& path) {
    if (!fs::exists(path)) return std::nullopt;
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open ba file");
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

vhsdecode::cppport::SosFilter read_sos_flat(const fs::path& path) {
    const auto flat = read_bin<double>(path);
    if ((flat.size() % 6U) != 0U) throw std::runtime_error("bad sos size");
    vhsdecode::cppport::SosFilter sos;
    for (std::size_t i = 0; i < flat.size(); i += 6) {
        sos.sections.push_back({flat[i + 0], flat[i + 1], flat[i + 2], flat[i + 3], flat[i + 4], flat[i + 5]});
    }
    return sos;
}

std::vector<std::vector<double>> read_phase_matrix(const fs::path& path) {
    const auto flat = read_bin<double>(path);
    if ((flat.size() % 4U) != 0U) throw std::runtime_error("heterodyne flat size must be divisible by 4");
    const std::size_t len = flat.size() / 4U;
    std::vector<std::vector<double>> out(4, std::vector<double>(len));
    for (std::size_t p = 0; p < 4; ++p) {
        std::copy(flat.begin() + static_cast<std::ptrdiff_t>(p * len),
                  flat.begin() + static_cast<std::ptrdiff_t>((p + 1) * len),
                  out[p].begin());
    }
    return out;
}

std::vector<vhsdecode_cpp::ValidPulse> read_valid_pulses(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open valid pulses csv");
    std::vector<vhsdecode_cpp::ValidPulse> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line == "type,start,valid") continue;
        std::istringstream iss(line);
        std::string tok;
        vhsdecode_cpp::ValidPulse vp{};
        std::getline(iss, tok, ',');
        vp.type = std::stoi(tok);
        std::getline(iss, tok, ',');
        vp.start = std::stoi(tok);
        std::getline(iss, tok, ',');
        vp.valid = (tok == "True" || tok == "true") ? 1 : (tok == "False" || tok == "false") ? 0 : std::stoi(tok);
        out.push_back(vp);
    }
    return out;
}

std::optional<std::vector<int>> read_rotation(const fs::path& path) {
    if (!fs::exists(path)) return std::nullopt;
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open rotation file");
    std::vector<int> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) out.push_back(std::stoi(line));
    }
    return out;
}

std::vector<vhsdecode_cpp::PhaseSequenceEntry> read_phase_sequence_csv(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("failed to open phase sequence csv");
    std::vector<vhsdecode_cpp::PhaseSequenceEntry> out;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {
            first = false;
            continue;
        }
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

double rms_abs_diff_u16(const std::vector<std::uint16_t>& a, const std::vector<std::uint16_t>& b) {
    if (a.size() != b.size()) throw std::runtime_error("u16 size mismatch");
    long double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const long double d = static_cast<long double>(a[i]) - static_cast<long double>(b[i]);
        sum += d * d;
    }
    return std::sqrt(static_cast<double>(sum / static_cast<long double>(a.size())));
}

double max_abs_diff_u16(const std::vector<std::uint16_t>& a, const std::vector<std::uint16_t>& b) {
    if (a.size() != b.size()) throw std::runtime_error("u16 size mismatch");
    std::uint16_t maxv = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto d = static_cast<unsigned>(std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
        if (d > maxv) maxv = static_cast<std::uint16_t>(d);
    }
    return static_cast<double>(maxv);
}

// Literal port of FieldNTSCShared._sync_to_burst().
void sync_to_burst(std::vector<double>& linelocs,
                   int outlinelen,
                   double burst_avg_phase,
                   const std::vector<vhsdecode_cpp::PhaseSequenceEntry>& phase_sequence) {
    for (std::size_t idx = 9; idx < phase_sequence.size(); ++idx) {
        const auto& row = phase_sequence[idx];
        const double phase_delta = std::fmod(burst_avg_phase - row.burst_phase + 180.0, 360.0) - 180.0;
        const int line_number = row.line_number;
        if (line_number < 0 || static_cast<std::size_t>(line_number + 1) >= linelocs.size()) continue;
        const double line_start = linelocs[static_cast<std::size_t>(line_number)];
        const double line_end = linelocs[static_cast<std::size_t>(line_number + 1)];
        const double line_length = line_end - line_start;
        const double scale = line_length / static_cast<double>(outlinelen);
        const double line_adjust = (phase_delta / 360.0) * 4.0;
        linelocs[static_cast<std::size_t>(line_number)] += line_adjust * scale;
    }
}

void apply_ntsc_phase_offset(std::vector<double>& linelocs, double sample_rate_mhz) {
    constexpr double kShift33Rad = 83.0 * (3.14159265358979323846 / 180.0);
    const double sample_shift = (-kShift33Rad) * (sample_rate_mhz / (4.0 * 315.0 / 88.0));
    for (double& loc : linelocs) loc += sample_shift;
}

struct SyncFixture {
    vhsdecode_cpp::FirstHsyncInput first_input{};
    vhsdecode_cpp::HsyncRefineInput refine_input{};
    double hsync_tolerance = 0.0;
    int proclines = 0;
    double gap_detection_threshold = 0.0;
};

struct PhaseFixture {
    vhsdecode_cpp::ChromaPhaseInput phase{};
    bool use_precomputed_phase = false;
    std::vector<vhsdecode_cpp::PhaseSequenceEntry> precomputed_phase_sequence;
    std::optional<double> precomputed_burst_phase_avg;
};

struct FieldFixture {
    vhsdecode_cpp::NtscVhsDownscaleInput input{};
    std::vector<std::uint16_t> python_luma;
    std::vector<std::uint16_t> python_chroma;
};

SyncFixture load_sync_fixture(const fs::path& dir) {
    const auto kv = read_kv(dir / "sync_config.kv");
    SyncFixture fix{};
    fix.first_input.validpulses = read_valid_pulses(dir / "validpulses.csv");
    fix.first_input.meanlinelen = parse_num<double>(kv, "meanlinelen");
    fix.first_input.is_ntsc = parse_num<int>(kv, "is_ntsc") != 0;
    fix.first_input.field_lines = read_bin<std::int32_t>(dir / "field_lines.i32");
    fix.first_input.num_eq_pulses = parse_num<int>(kv, "num_eq_pulses");
    fix.first_input.prev_first_field = parse_num<int>(kv, "prev_first_field");
    fix.first_input.last_field_offset_lines = parse_num<double>(kv, "last_field_offset_lines");
    fix.first_input.prev_first_hsync_loc = parse_num<double>(kv, "prev_first_hsync_loc");
    fix.first_input.prev_hsync_diff = parse_num<double>(kv, "prev_hsync_diff");
    fix.first_input.field_order_confidence = parse_num<int>(kv, "field_order_confidence");
    fix.first_input.fallback_line0loc = parse_num<double>(kv, "fallback_line0loc");
    fix.first_input.fallback_is_first_field = parse_num<int>(kv, "fallback_is_first_field");
    fix.first_input.fallback_is_first_field_confidence = parse_num<int>(kv, "fallback_is_first_field_confidence");

    fix.refine_input.linelocs1 = read_bin<double>(dir / "linelocs1.f64");
    fix.refine_input.demod_05 = read_bin<double>(dir / "demod_05.f64");
    fix.refine_input.normal_hsync_length = parse_num<int>(kv, "normal_hsync_length");
    fix.refine_input.one_usec = parse_num<int>(kv, "one_usec");
    fix.refine_input.sample_rate_mhz = parse_num<double>(kv, "sample_rate_mhz");
    fix.refine_input.is_pal = parse_num<int>(kv, "is_pal") != 0;
    fix.refine_input.disable_right_hsync = parse_num<int>(kv, "disable_right_hsync") != 0;
    fix.refine_input.hsync_threshold = parse_num<double>(kv, "hsync_threshold");
    fix.refine_input.ire_30 = parse_num<double>(kv, "ire_30");
    fix.refine_input.ire_n_65 = parse_num<double>(kv, "ire_n_65");
    fix.refine_input.ire_110 = parse_num<double>(kv, "ire_110");
    fix.hsync_tolerance = parse_num<double>(kv, "hsync_tolerance");
    fix.proclines = parse_num<int>(kv, "proclines");
    fix.gap_detection_threshold = parse_num<double>(kv, "gap_detection_threshold");
    return fix;
}

PhaseFixture load_phase_fixture(const fs::path& dir) {
    PhaseFixture fix{};
    if (!fs::exists(dir / "phase_config.kv")) {
        const auto pkv = read_kv(dir / "process_config.kv");
        fix.use_precomputed_phase = true;
        fix.precomputed_phase_sequence = read_phase_sequence_csv(dir / "python_phase_sequence.csv");
        if (parse_num<int>(pkv, "burst_phase_avg_present") != 0) {
            fix.precomputed_burst_phase_avg = parse_num<double>(pkv, "burst_phase_avg");
        }
        return fix;
    }
    const auto kv = read_kv(dir / "phase_config.kv");
    fix.phase.chroma = read_bin<double>(dir / "chroma_downscaled.f64");
    fix.phase.chroma_heterodyne = read_phase_matrix(dir / "chroma_heterodyne_flat.f64");
    fix.phase.chroma_filter = read_sos_flat(dir / "fchroma_final_sos.f64");
    fix.phase.chroma_rotation = read_rotation(dir / "chroma_rotation.txt");
    if (parse_num<int>(kv, "track_phase_present") != 0) {
        fix.phase.chroma_rotation_index = parse_num<int>(kv, "track_phase");
    }
    fix.phase.lineoffset = parse_num<int>(kv, "lineoffset");
    fix.phase.linesout = parse_num<int>(kv, "linesout");
    fix.phase.outwidth = parse_num<int>(kv, "outwidth");
    fix.phase.burst_start = parse_num<int>(kv, "burst_start");
    fix.phase.burst_end = parse_num<int>(kv, "burst_end");
    fix.phase.burst_sin = read_bin<double>(dir / "fsc_wave.f64");
    fix.phase.burst_cos = read_bin<double>(dir / "fsc_cos_wave.f64");
    fix.phase.detect_chroma_track_phase = parse_num<int>(kv, "detect_chroma_track_phase") != 0;
    fix.phase.rotation_check_start_line = parse_num<int>(kv, "rotation_check_start_line");
    fix.phase.is_ntsc = parse_num<int>(kv, "is_ntsc") != 0;
    return fix;
}

FieldFixture load_field_fixture(const fs::path& dir) {
    const auto dkv = read_kv(dir / "downscale_config.kv");
    const auto pkv = read_kv(dir / "process_config.kv");
    FieldFixture fix{};
    fix.input.write_chroma = true;

    fix.input.luma.demod = read_bin<float>(dir / "demod.f32");
    fix.input.luma.linelocs = read_bin<double>(dir / "linelocs.f64");
    fix.input.luma.lineoffset = parse_num<int>(dkv, "lineoffset");
    fix.input.luma.outlinecount = parse_num<int>(dkv, "outlinecount");
    fix.input.luma.outlinelen = parse_num<int>(dkv, "outlinelen");
    fix.input.luma.inlinelen = parse_num<double>(dkv, "inlinelen");
    fix.input.luma.wow_level_adjust_smoothing = parse_num<double>(dkv, "wow_level_adjust_smoothing");
    fix.input.luma.y_comb_limit = parse_num<double>(dkv, "y_comb_limit");
    fix.input.luma.final_output = parse_num<int>(dkv, "final_output") != 0;
    fix.input.luma.export_raw_tbc = parse_num<int>(dkv, "export_raw_tbc") != 0;
    fix.input.luma.ire0 = parse_num<double>(dkv, "ire0");
    fix.input.luma.hz_ire = parse_num<double>(dkv, "hz_ire");
    fix.input.luma.output_zero = parse_num<double>(dkv, "output_zero");
    fix.input.luma.vsync_ire = parse_num<double>(dkv, "vsync_ire");
    fix.input.luma.out_scale = parse_num<double>(dkv, "out_scale");
    fix.input.chroma.chroma = read_bin<double>(dir / "chroma_downscaled.f64");
    fix.input.chroma.chroma_heterodyne = read_phase_matrix(dir / "chroma_heterodyne_flat.f64");
    fix.input.chroma.phase_sequence = {};
    fix.input.chroma.fchroma_final = read_sos_flat(dir / "fchroma_final_sos.f64");
    fix.input.chroma.fvideo_notch = read_ba_optional(dir / "fvideo_notch.ba");
    fix.input.chroma.chroma_audio_notch = read_ba_optional(dir / "chroma_audio_notch.ba");
    fix.input.chroma.chroma_deemphasis = read_ba_optional(dir / "chroma_deemphasis.ba");
    fix.input.chroma.lineoffset = parse_num<int>(pkv, "lineoffset");
    fix.input.chroma.linesout = parse_num<int>(pkv, "linesout");
    fix.input.chroma.outwidth = parse_num<int>(pkv, "outwidth");
    fix.input.chroma.burst_start = parse_num<int>(pkv, "burst_start");
    fix.input.chroma.burst_end = parse_num<int>(pkv, "burst_end");
    fix.input.chroma.cafc_move = parse_num<int>(pkv, "cafc_move");
    fix.input.chroma.is_ntsc = parse_num<int>(pkv, "is_ntsc") != 0;
    fix.input.chroma.do_cafc = parse_num<int>(pkv, "do_cafc") != 0;
    fix.input.chroma.disable_deemph = parse_num<int>(pkv, "disable_deemph") != 0;
    fix.input.chroma.disable_comb = parse_num<int>(pkv, "disable_comb") != 0;
    fix.input.chroma.disable_tracking_cafc = parse_num<int>(pkv, "disable_tracking_cafc") != 0;
    fix.input.chroma.disable_phase_correction = parse_num<int>(pkv, "disable_phase_correction") != 0;
    fix.input.chroma.do_chroma_deemphasis = parse_num<int>(pkv, "do_chroma_deemphasis") != 0;
    fix.input.chroma.enable_video_notch = parse_num<int>(pkv, "enable_video_notch") != 0;
    if (parse_num<int>(pkv, "burst_phase_avg_present") != 0) {
        fix.input.chroma.burst_phase_avg = parse_num<double>(pkv, "burst_phase_avg");
    }
    fix.input.chroma.burst_abs_ref = parse_num<double>(pkv, "burst_abs_ref");
    const fs::path luma_a = dir / "python_field_dsout_u16.u16";
    const fs::path luma_b = dir / "python_dsout_u16.u16";
    fix.python_luma = read_bin<std::uint16_t>(fs::exists(luma_a) ? luma_a : luma_b);
    fix.python_chroma = read_bin<std::uint16_t>(dir / "python_field_chroma_u16.u16");
    return fix;
}

struct RunResult {
    std::vector<double> linelocs;
    vhsdecode_cpp::ChromaPhaseResult phase{};
    vhsdecode_cpp::NtscVhsDownscaleResult field{};
};

RunResult run_once(const SyncFixture& sync,
                   const PhaseFixture& phase,
                   const FieldFixture& field,
                   bool use_field_linelocs) {
    RunResult out{};
    if (use_field_linelocs) {
        out.linelocs = field.input.luma.linelocs;
    } else {
        const auto first = vhsdecode_cpp::get_first_hsync_loc(sync.first_input);
        std::vector<double> pulse_starts;
        pulse_starts.reserve(sync.first_input.validpulses.size());
        for (const auto& p : sync.first_input.validpulses) pulse_starts.push_back(static_cast<double>(p.start));
        const auto lineloc0 = vhsdecode_cpp::valid_pulses_to_linelocs(
            pulse_starts,
            static_cast<int>(std::lround(first.first_hsync_loc)),
            static_cast<int>(std::lround(first.hsync_start_line)),
            sync.first_input.meanlinelen,
            sync.hsync_tolerance,
            sync.proclines,
            sync.gap_detection_threshold);
        (void)lineloc0;
        std::vector<std::uint8_t> linebad(sync.refine_input.linelocs1.size(), 0);
        out.linelocs = vhsdecode_cpp::refine_linelocs_hsync(sync.refine_input, linebad);
        if (!out.linelocs.empty() && !field.input.luma.linelocs.empty()) {
            const double origin_delta = field.input.luma.linelocs.front() - out.linelocs.front();
            for (double& loc : out.linelocs) loc += origin_delta;
        }
    }
    if (phase.use_precomputed_phase) {
        out.phase.track_phase = 0;
        out.phase.phase_sequence = phase.precomputed_phase_sequence;
        out.phase.burst_phase_avg = phase.precomputed_burst_phase_avg;
    } else {
        out.phase = vhsdecode_cpp::get_phase_rotation_sequence(phase.phase);
    }
    if (!use_field_linelocs && out.phase.burst_phase_avg.has_value()) {
        sync_to_burst(out.linelocs, field.input.luma.outlinelen, *out.phase.burst_phase_avg, out.phase.phase_sequence);
    }
    if (!use_field_linelocs) {
        apply_ntsc_phase_offset(out.linelocs, sync.refine_input.sample_rate_mhz);
    }

    auto integrated = field.input;
    integrated.luma.linelocs = out.linelocs;
    integrated.chroma.phase_sequence = out.phase.phase_sequence;
    integrated.chroma.burst_phase_avg = out.phase.burst_phase_avg;
    out.field = vhsdecode_cpp::downscale_ntsc_vhs(integrated);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4 || argc > 6) {
        std::cerr << "usage: vhsdecode_ntsc_vhs_hotpath <k2_dir|-> <k3_phase_dir> <k4_field_dir> [bench_iters] [--use-field-linelocs]\n";
        return 2;
    }
    const fs::path k2_dir = argv[1];
    const fs::path k3_dir = argv[2];
    const fs::path k4_dir = argv[3];
    int bench_iters = 200;
    bool use_field_linelocs = false;
    for (int i = 4; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--use-field-linelocs") {
            use_field_linelocs = true;
        } else {
            bench_iters = std::stoi(arg);
        }
    }

    SyncFixture sync{};
    if (!use_field_linelocs && k2_dir != "-") {
        sync = load_sync_fixture(k2_dir);
    }
    const auto phase = load_phase_fixture(k3_dir);
    const auto field = load_field_fixture(k4_dir);

    using clock = std::chrono::steady_clock;
    double total_s = 0.0;
    RunResult last{};
    for (int i = 0; i < bench_iters; ++i) {
        const auto t0 = clock::now();
        last = run_once(sync, phase, field, use_field_linelocs);
        const auto t1 = clock::now();
        total_s += std::chrono::duration<double>(t1 - t0).count();
    }
    const double avg_s = total_s / static_cast<double>(bench_iters);
    const double luma_rms = rms_abs_diff_u16(last.field.luma.dsout_u16, field.python_luma);
    const double luma_max = max_abs_diff_u16(last.field.luma.dsout_u16, field.python_luma);
    const double chroma_rms = rms_abs_diff_u16(last.field.chroma.chroma_u16, field.python_chroma);
    const double chroma_max = max_abs_diff_u16(last.field.chroma.chroma_u16, field.python_chroma);

    std::cout << std::setprecision(17)
              << "{\n"
              << "  \"bench_iterations\": " << bench_iters << ",\n"
              << "  \"avg_compute_s\": " << avg_s << ",\n"
              << "  \"throughput_per_s\": " << (avg_s > 0.0 ? 1.0 / avg_s : 0.0) << ",\n"
              << "  \"luma_u16_max_abs\": " << luma_max << ",\n"
              << "  \"luma_u16_rms\": " << luma_rms << ",\n"
              << "  \"luma_u16_rms_pct_fullscale\": " << (luma_rms / 65535.0) * 100.0 << ",\n"
              << "  \"chroma_u16_max_abs\": " << chroma_max << ",\n"
              << "  \"chroma_u16_rms\": " << chroma_rms << ",\n"
              << "  \"chroma_u16_rms_pct_fullscale\": " << (chroma_rms / 65535.0) * 100.0 << "\n"
              << "}\n";
    return 0;
}

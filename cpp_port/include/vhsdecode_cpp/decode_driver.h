#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace vhsdecode_cpp {

enum class DecodeProfile {
    NtscVhs,
};

struct DecodeDriverConfig {
    DecodeProfile profile = DecodeProfile::NtscVhs;
    std::filesystem::path input_u8;
    std::filesystem::path output_base;
    std::filesystem::path metadata_json;
    std::filesystem::path k1_dir;
    std::filesystem::path k3_phase_dir;
    std::filesystem::path k4_dir;
    std::filesystem::path debug_dump_dir;
    int debug_capture_seq = 1;
    int max_fields = -1;
    int max_attempts = -1;
    int threads = 1;
    bool verbose = true;
};

struct DecodeDriverResult {
    std::string input;
    std::string output_base;
    int fields_seen = 0;
    int attempts = 0;
    int fields_written = 0;
    double elapsed_s = 0.0;
    double throughput_fields_per_s = 0.0;
    double throughput_frames_per_s = 0.0;
};

// Shared top-level decode-session runner.
//
// !!! LOUD ARCHITECTURE NOTE !!!
// This is intentionally the shared session driver layer. Format-specific
// behavior belongs in profile/config objects underneath this API, not in
// separate top-level binaries.
DecodeDriverResult run_decode_driver(const DecodeDriverConfig& config);

}  // namespace vhsdecode_cpp

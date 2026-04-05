#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace vhsdecode_cpp {

struct WriteoutState {
    std::vector<nlohmann::json> fieldinfo;
    std::size_t fields_written = 0;
    std::size_t video_bytes_written = 0;
    std::size_t chroma_bytes_written = 0;
    bool write_chroma = true;
};

struct WriteoutInput {
    nlohmann::json field_metadata;
    std::size_t picturey_bytes = 0;
    std::size_t picturec_bytes = 0;
};

struct BuildJsonInput {
    int analog_audio = 0;
    std::string os_info;
    std::string git_branch;
    std::string git_commit;
    std::string system;
    int field_width = 0;
    double sample_rate = 0.0;
    double black16b_ire = 0.0;
    double white16b_ire = 0.0;
    int field_height = 0;
    int colour_burst_start = 0;
    int colour_burst_end = 0;
    int active_video_start = 0;
    int active_video_end = 0;
    double level_adjust = 0.0;
    std::string color_system;
    std::string tape_format;
};

// VHSDecode.writeout() removes currently-unused audioSamples metadata, appends
// the field metadata, and writes luma/chroma payloads according to write_chroma.
void write_field_dataset(const WriteoutInput& input, WriteoutState& state);

// !!! LOUD FUTURE-WORK MARKER !!!
// This mirrors the exercised VHS/NTSC JSON contract exactly. If we later add
// richer output manifests or automatic format-specific policy here, that should
// be layered on top of this parity-first contract rather than replacing it.
nlohmann::json build_vhs_json(const BuildJsonInput& input, const WriteoutState& state);

}  // namespace vhsdecode_cpp

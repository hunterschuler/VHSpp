#pragma once

#include <optional>
#include <string>
#include <vector>

namespace vhsdecode_cpp {

struct DropoutInfo {
    std::vector<int> field_line;
    std::vector<int> startx;
    std::vector<int> endx;
};

struct MetadataInput {
    bool is_first_field = false;
    bool detected_first_field = false;
    int sync_conf = 0;
    int seq_no = 0;
    double disk_loc = 0.0;
    int file_loc = 0;
    std::optional<int> field_phase_id;
    std::optional<DropoutInfo> dropouts;
    std::string vits_metrics_json;
};

struct MetadataOutput {
    bool is_first_field = false;
    bool detected_first_field = false;
    bool is_duplicate_field = false;
    int sync_conf = 0;
    int seq_no = 0;
    double disk_loc = 0.0;
    int file_loc = 0;
    int field_phase_id = 0;
    std::optional<DropoutInfo> dropouts;
    std::string vits_metrics_json;
    std::optional<int> decode_faults;
    bool write_field = true;
};

struct MetadataState {
    std::vector<MetadataOutput> fieldinfo;
    std::string field_order_action = "detect";
    bool duplicate_prev_field = true;
    bool do_dod = true;
    bool typec_mode = false;
};

// Literal-first port of the exercised VHSDecode.buildmetadata() state machine.
//
// !!! LOUD FUTURE-WORK MARKER !!!
// This stage is where field-order heuristics and duplicate/drop policy become
// user-visible. A future "smarter" policy engine may be worthwhile, but for
// parity we intentionally preserve the explicit upstream behaviors first.
MetadataOutput build_metadata(const MetadataInput& input, MetadataState& state);

}  // namespace vhsdecode_cpp

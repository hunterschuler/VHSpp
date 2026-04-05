#pragma once

#include <cstddef>
#include <optional>

#include <nlohmann/json.hpp>

#include "vhsdecode_cpp/output_core.h"

namespace vhsdecode_cpp {

struct SessionFieldCase {
    bool is_first_field = false;
    bool duplicate_field = false;
    bool write_field = false;
    int readloc = 0;
    nlohmann::json field_metadata;
    std::size_t picturey_bytes = 0;
    std::size_t picturec_bytes = 0;
};

struct SessionState {
    WriteoutState output_state;
    std::optional<SessionFieldCase> lastvalid_false;
    std::optional<SessionFieldCase> lastvalid_true;
    std::optional<std::pair<std::size_t, int>> last_field_written;
};

struct SessionStepResult {
    bool wrote_anything = false;
    std::size_t writes_performed = 0;
};

// Literal port of the write-orchestration tail of VHSDecode.readfield():
// - suppress leading second-field output
// - cache last valid field by parity
// - handle duplicate filler writes
// - update lastFieldWritten and output streams
SessionStepResult process_session_field(const SessionFieldCase& input, SessionState& state);

}  // namespace vhsdecode_cpp

#pragma once

#include <optional>

namespace vhsdecode_cpp {

struct ReadfieldControlInput {
    bool field_present = false;
    bool field_valid = false;
    bool needrerun = false;
    bool use_agc = false;
    bool is_first_field = false;
    double sync_confidence = 0.0;
    int fields_written = 0;
    int fdoffset = 0;
    int offset = 0;
    bool offset_is_none = false;
    bool adjusted = false;

    double sync_hz = 0.0;
    double ire0_hz = 0.0;
    double ire100_hz = 0.0;
    double actualwhite_ire = 100.0;
    double hz_to_ire_sync = 0.0;
    double hz_to_ire_ire0 = 0.0;
    double hz_to_ire_ire100 = 100.0;
    double current_vsync_ire = -40.0;
};

struct ReadfieldControlOutput {
    int next_fdoffset = 0;
    bool adjusted = false;
    bool redo = false;
    int redo_target = 0;
    bool flush_demod = false;
    bool insert_none = false;
    bool insert_field = false;
    bool done = false;

    bool agc_updated = false;
    double updated_ire0_hz = 0.0;
    double updated_hz_ire = 0.0;
    double updated_vsync_ire = 0.0;
};

// Literal-first port of the VHS-specific readfield() acceptance/rerun control.
//
// !!! LOUD NOTE !!!
// Upstream VHSDecode.checkMTF() is hardwired true, so K7 for the exercised VHS
// path is about needrerun + AGC reruns, not lddecode's broader MTF policy.
ReadfieldControlOutput evaluate_readfield_control(const ReadfieldControlInput& in);

}  // namespace vhsdecode_cpp

#include "vhsdecode_cpp/session_core.h"

namespace vhsdecode_cpp {
namespace {

void do_write(const SessionFieldCase& input, SessionState& state) {
    WriteoutInput w{};
    w.field_metadata = input.field_metadata;
    w.picturey_bytes = input.picturey_bytes;
    w.picturec_bytes = input.picturec_bytes;
    write_field_dataset(w, state.output_state);
}

}  // namespace

SessionStepResult process_session_field(const SessionFieldCase& input, SessionState& state) {
    SessionStepResult result{};

    if (state.output_state.fieldinfo.empty() && !input.is_first_field) {
        return result;
    }

    if (input.write_field) {
        if (input.is_first_field) {
            state.lastvalid_true = input;
        } else {
            state.lastvalid_false = input;
        }
    }

    if (input.duplicate_field) {
        const auto& other = input.is_first_field ? state.lastvalid_false : state.lastvalid_true;
        const auto& self = input.is_first_field ? state.lastvalid_true : state.lastvalid_false;
        if (other.has_value()) {
            do_write(*other, state);
            result.writes_performed += 1;
            result.wrote_anything = true;
            if (self.has_value()) {
                do_write(*self, state);
                result.writes_performed += 1;
            }
        }
        return result;
    }

    if (input.write_field) {
        state.last_field_written = std::make_pair(state.output_state.fields_written, input.readloc);
        const auto& self = input.is_first_field ? state.lastvalid_true : state.lastvalid_false;
        if (self.has_value()) {
            do_write(*self, state);
            result.writes_performed = 1;
            result.wrote_anything = true;
        }
    }

    return result;
}

}  // namespace vhsdecode_cpp

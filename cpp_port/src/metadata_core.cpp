#include "vhsdecode_cpp/metadata_core.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace vhsdecode_cpp {
namespace {

bool in_range(double v, double lo, double hi) { return v >= lo && v <= hi; }

int fallback_field_phase_id(bool is_first_field, int seq_no) {
    const int half_mod = (seq_no / 2) % 2;
    if (is_first_field && half_mod == 0) return 1;
    if (!is_first_field && half_mod == 1) return 2;
    if (is_first_field && half_mod == 1) return 3;
    return 4;
}

}  // namespace

MetadataOutput build_metadata(const MetadataInput& input, MetadataState& state) {
    MetadataOutput out{};
    out.is_first_field = input.is_first_field;
    out.detected_first_field = input.detected_first_field;
    out.is_duplicate_field = false;
    out.sync_conf = input.sync_conf;
    out.seq_no = input.seq_no;
    out.disk_loc = input.disk_loc;
    out.file_loc = input.file_loc;
    out.field_phase_id = input.field_phase_id.has_value()
        ? *input.field_phase_id
        : fallback_field_phase_id(out.is_first_field, out.seq_no);
    if (state.do_dod && input.dropouts.has_value()) out.dropouts = input.dropouts;
    out.vits_metrics_json = input.vits_metrics_json;

    const MetadataOutput* prev1 = state.fieldinfo.empty() ? nullptr : &state.fieldinfo.back();
    const MetadataOutput* prev2 = (state.fieldinfo.size() > 1) ? &state.fieldinfo[state.fieldinfo.size() - 2U] : nullptr;

    int decode_faults = 0;
    bool write_field = true;

    if (prev1 != nullptr && prev1->is_first_field == out.is_first_field) {
        const double distance = out.disk_loc - prev1->disk_loc;
        if (prev1->detected_first_field == out.detected_first_field &&
            prev2 != nullptr &&
            prev2->detected_first_field == prev1->detected_first_field &&
            in_range(distance, 0.9, 1.1) &&
            !state.typec_mode) {
            decode_faults |= 1;
            out.sync_conf = 10;
            out.is_first_field = !prev1->is_first_field;
        } else {
            if (state.field_order_action == "duplicate") {
                state.duplicate_prev_field = true;
            } else if (state.field_order_action == "drop") {
                state.duplicate_prev_field = false;
            } else if (state.field_order_action == "detect") {
                if (distance > 1.1) state.duplicate_prev_field = true;
                else if (distance < 0.9) state.duplicate_prev_field = false;
                else state.duplicate_prev_field = !state.duplicate_prev_field;
            }

            if (state.field_order_action == "none") {
                decode_faults |= 4;
                out.sync_conf = 0;
                out.is_first_field = !prev1->is_first_field;
            } else if (state.duplicate_prev_field) {
                decode_faults |= 4;
                out.sync_conf = 0;
                out.is_duplicate_field = true;
            } else {
                decode_faults |= 4;
                out.sync_conf = 0;
                write_field = false;
            }
        }
    }

    if (decode_faults != 0) out.decode_faults = decode_faults;
    out.write_field = write_field;
    return out;
}

}  // namespace vhsdecode_cpp

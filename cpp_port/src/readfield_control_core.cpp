#include "vhsdecode_cpp/readfield_control_core.h"

#include <algorithm>
#include <cmath>

namespace vhsdecode_cpp {
namespace {

double nb_abs(double v) { return std::abs(v); }

}  // namespace

ReadfieldControlOutput evaluate_readfield_control(const ReadfieldControlInput& in) {
    ReadfieldControlOutput out{};
    out.next_fdoffset = in.fdoffset;
    out.adjusted = in.adjusted;

    if (in.field_present) {
        out.next_fdoffset += in.offset;
    } else if (in.offset_is_none) {
        out.insert_none = true;
    }

    if (!(in.field_present && in.field_valid)) {
        return out;
    }

    bool redo = in.needrerun;
    int redo_target = 0;
    if (redo) {
        redo_target = in.fdoffset - in.offset;
    }

    if (in.use_agc && in.is_first_field && in.sync_confidence > 80.0) {
        const double sync_ire_diff = nb_abs(in.hz_to_ire_sync - in.current_vsync_ire);
        const double whitediff = nb_abs(in.hz_to_ire_ire100 - in.actualwhite_ire);
        const double ire0_diff = nb_abs(in.hz_to_ire_ire0);
        const double acceptable_diff = in.fields_written ? 2.0 : 0.5;

        if (std::max({whitediff, ire0_diff, sync_ire_diff}) > acceptable_diff) {
            const double hz_ire = (in.ire100_hz - in.ire0_hz) / 100.0;
            const double vsync_ire = (in.sync_hz - in.ire0_hz) / hz_ire;
            if (vsync_ire <= -20.0) {
                redo = true;
                redo_target = in.fdoffset - in.offset;
                out.agc_updated = true;
                out.updated_ire0_hz = in.ire0_hz;
                out.updated_hz_ire = hz_ire;
                out.updated_vsync_ire = vsync_ire;
            }
        }
    }

    out.redo = redo;
    out.redo_target = redo ? redo_target : 0;

    if (!in.adjusted && redo) {
        out.flush_demod = true;
        out.adjusted = true;
        out.next_fdoffset = redo_target;
    } else {
        out.done = true;
        out.insert_field = true;
    }

    return out;
}

}  // namespace vhsdecode_cpp

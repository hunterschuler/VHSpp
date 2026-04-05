#include "vhsdecode_cpp/session_loop_core.h"

namespace vhsdecode_cpp {

StartupControlOutput evaluate_startup_control(const StartupControlInput& in) {
    StartupControlOutput out{};
    if (in.start_fileloc != -1) {
        out.roughseek_target = in.start_fileloc;
        out.roughseek_is_fileloc = true;
    } else {
        out.roughseek_target = in.firstframe * 2;
        out.roughseek_is_fileloc = false;
    }

    if (in.system == "NTSC" && !in.ntscj) {
        out.set_black_ire = true;
        out.black_ire_value = 7.5;
    }
    return out;
}

SessionLoopStepOutput evaluate_session_loop_step(const SessionLoopStepInput& in) {
    SessionLoopStepOutput out{};
    out.done = in.done || in.field_is_none;
    out.should_clear_prevfield = !in.field_is_none;
    out.should_jsondump_write = in.fields_written < 100 || ((in.fields_written % 500) == 0);
    out.should_check_disk = out.should_jsondump_write && !in.disk_usage_raises;
    out.should_pause_for_disk = out.should_check_disk && (in.free_space_bytes < (10LL * 1024LL * 1024LL * 1024LL));
    return out;
}

SessionLoopFinalizeOutput evaluate_session_loop_finalize(const SessionLoopFinalizeInput& in) {
    SessionLoopFinalizeOutput out{};
    if (in.fields_written) {
        out.message = "\nCompleted: saving JSON and exiting.";
    } else {
        out.message = "\nCompleted without handling any frames.";
    }
    out.exit_code = 0;
    return out;
}

}  // namespace vhsdecode_cpp

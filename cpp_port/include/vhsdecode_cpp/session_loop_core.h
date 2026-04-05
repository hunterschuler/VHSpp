#pragma once

#include <optional>
#include <string>

namespace vhsdecode_cpp {

struct StartupControlInput {
    int start_fileloc = -1;
    int firstframe = 0;
    std::string system = "NTSC";
    bool ntscj = false;
};

struct StartupControlOutput {
    int roughseek_target = 0;
    bool roughseek_is_fileloc = false;
    bool set_black_ire = false;
    double black_ire_value = 0.0;
};

struct SessionLoopStepInput {
    bool done = false;
    int fields_written = 0;
    int req_frames = 0;
    bool field_is_none = false;
    bool disk_usage_raises = false;
    long long free_space_bytes = 0;
};

struct SessionLoopStepOutput {
    bool done = false;
    bool should_clear_prevfield = false;
    bool should_jsondump_write = false;
    bool should_check_disk = false;
    bool should_pause_for_disk = false;
};

struct SessionLoopFinalizeInput {
    int fields_written = 0;
};

struct SessionLoopFinalizeOutput {
    std::string message;
    int exit_code = 0;
};

StartupControlOutput evaluate_startup_control(const StartupControlInput& in);
SessionLoopStepOutput evaluate_session_loop_step(const SessionLoopStepInput& in);
SessionLoopFinalizeOutput evaluate_session_loop_finalize(const SessionLoopFinalizeInput& in);

}  // namespace vhsdecode_cpp

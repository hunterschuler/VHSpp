#include "vhsdecode_cpp/session_loop_core.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc != 2) throw std::runtime_error("usage: vhsdecode_k8_session_loop_parity <parity_dir>");
    const fs::path dir = argv[1];
    std::ifstream in(dir / "session_loop_cases.json");
    if (!in.is_open()) throw std::runtime_error("failed to open session_loop_cases.json");
    json root;
    in >> root;

    const auto& s = root.at("startup_input");
    vhsdecode_cpp::StartupControlInput si{};
    si.start_fileloc = s.at("start_fileloc").get<int>();
    si.firstframe = s.at("firstframe").get<int>();
    si.system = s.at("system").get<std::string>();
    si.ntscj = s.at("ntscj").get<bool>();
    const auto so = vhsdecode_cpp::evaluate_startup_control(si);

    json step_rows = json::array();
    for (const auto& row : root.at("steps")) {
        vhsdecode_cpp::SessionLoopStepInput i{};
        i.done = row.at("done").get<bool>();
        i.fields_written = row.at("fields_written").get<int>();
        i.req_frames = row.at("req_frames").get<int>();
        i.field_is_none = row.at("field_is_none").get<bool>();
        i.disk_usage_raises = row.at("disk_usage_raises").get<bool>();
        i.free_space_bytes = row.at("free_space_bytes").get<long long>();
        const auto o = vhsdecode_cpp::evaluate_session_loop_step(i);
        step_rows.push_back({
            {"done", o.done},
            {"should_clear_prevfield", o.should_clear_prevfield},
            {"should_jsondump_write", o.should_jsondump_write},
            {"should_check_disk", o.should_check_disk},
            {"should_pause_for_disk", o.should_pause_for_disk},
        });
    }

    const auto& f = root.at("finalize_input");
    vhsdecode_cpp::SessionLoopFinalizeInput fi{};
    fi.fields_written = f.at("fields_written").get<int>();
    const auto fo = vhsdecode_cpp::evaluate_session_loop_finalize(fi);

    json out;
    out["startup"] = {
        {"roughseek_target", so.roughseek_target},
        {"roughseek_is_fileloc", so.roughseek_is_fileloc},
        {"set_black_ire", so.set_black_ire},
        {"black_ire_value", so.black_ire_value},
    };
    out["steps"] = step_rows;
    out["finalize"] = {
        {"message", fo.message},
        {"exit_code", fo.exit_code},
    };

    std::ofstream fout(dir / "cpp_session_loop_result.json");
    fout << out.dump(2);
    return 0;
}

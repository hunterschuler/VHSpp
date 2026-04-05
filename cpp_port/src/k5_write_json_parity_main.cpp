#include "vhsdecode_cpp/output_core.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: vhsdecode_k5_write_json_parity <parity_dir>\n";
        return 2;
    }

    const fs::path dir = argv[1];
    std::ifstream in(dir / "write_json_cases.json");
    if (!in.is_open()) {
        throw std::runtime_error("failed to open write_json_cases.json");
    }

    json root;
    in >> root;

    vhsdecode_cpp::WriteoutState state{};
    state.write_chroma = root.at("write_chroma").get<bool>();

    for (const auto& row : root.at("cases")) {
        vhsdecode_cpp::WriteoutInput input{};
        input.field_metadata = row.at("field_metadata");
        input.picturey_bytes = row.at("picturey_bytes").get<std::size_t>();
        input.picturec_bytes = row.at("picturec_bytes").get<std::size_t>();
        vhsdecode_cpp::write_field_dataset(input, state);
    }

    const auto& jcfg = root.at("build_json_input");
    vhsdecode_cpp::BuildJsonInput cfg{};
    cfg.analog_audio = jcfg.at("analog_audio").get<int>();
    cfg.os_info = jcfg.at("os_info").get<std::string>();
    cfg.git_branch = jcfg.at("git_branch").get<std::string>();
    cfg.git_commit = jcfg.at("git_commit").get<std::string>();
    cfg.system = jcfg.at("system").get<std::string>();
    cfg.field_width = jcfg.at("field_width").get<int>();
    cfg.sample_rate = jcfg.at("sample_rate").get<double>();
    cfg.black16b_ire = jcfg.at("black16bIre").get<double>();
    cfg.white16b_ire = jcfg.at("white16bIre").get<double>();
    cfg.field_height = jcfg.at("field_height").get<int>();
    cfg.colour_burst_start = jcfg.at("colourBurstStart").get<int>();
    cfg.colour_burst_end = jcfg.at("colourBurstEnd").get<int>();
    cfg.active_video_start = jcfg.at("activeVideoStart").get<int>();
    cfg.active_video_end = jcfg.at("activeVideoEnd").get<int>();
    cfg.level_adjust = jcfg.at("level_adjust").get<double>();
    cfg.color_system = jcfg.at("color_system").get<std::string>();
    cfg.tape_format = jcfg.at("tape_format").get<std::string>();

    json out;
    out["fieldinfo"] = state.fieldinfo;
    out["fields_written"] = state.fields_written;
    out["video_bytes_written"] = state.video_bytes_written;
    out["chroma_bytes_written"] = state.chroma_bytes_written;
    out["build_json"] = vhsdecode_cpp::build_vhs_json(cfg, state);

    std::ofstream fout(dir / "cpp_write_json_result.json");
    fout << out.dump(2);
    return 0;
}

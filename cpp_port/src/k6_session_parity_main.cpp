#include "vhsdecode_cpp/session_core.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc != 2) {
        throw std::runtime_error("usage: vhsdecode_k6_session_parity <parity_dir>");
    }

    const fs::path dir = argv[1];
    std::ifstream in(dir / "session_cases.json");
    if (!in.is_open()) {
        throw std::runtime_error("failed to open session_cases.json");
    }
    json root;
    in >> root;

    vhsdecode_cpp::SessionState state{};
    state.output_state.write_chroma = root.at("write_chroma").get<bool>();

    json steps = json::array();
    for (const auto& row : root.at("cases")) {
        vhsdecode_cpp::SessionFieldCase c{};
        c.is_first_field = row.at("isFirstField").get<bool>();
        c.duplicate_field = row.at("duplicateField").get<bool>();
        c.write_field = row.at("writeField").get<bool>();
        c.readloc = row.at("readloc").get<int>();
        c.field_metadata = row.at("field_metadata");
        c.picturey_bytes = row.at("picturey_bytes").get<std::size_t>();
        c.picturec_bytes = row.at("picturec_bytes").get<std::size_t>();
        const auto step = vhsdecode_cpp::process_session_field(c, state);
        json out = {
            {"writes_performed", step.writes_performed},
            {"fields_written", state.output_state.fields_written},
            {"video_bytes_written", state.output_state.video_bytes_written},
            {"chroma_bytes_written", state.output_state.chroma_bytes_written},
            {"fieldinfo", state.output_state.fieldinfo},
        };
        if (state.last_field_written.has_value()) {
            out["lastFieldWritten"] = {
                state.last_field_written->first,
                state.last_field_written->second,
            };
        } else {
            out["lastFieldWritten"] = nullptr;
        }
        steps.push_back(out);
    }

    std::ofstream fout(dir / "cpp_session_result.json");
    fout << steps.dump(2);
    return 0;
}

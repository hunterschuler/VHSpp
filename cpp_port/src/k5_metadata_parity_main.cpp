#include "vhsdecode_cpp/metadata_core.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::optional<vhsdecode_cpp::DropoutInfo> parse_dropouts(const json& j) {
    if (j.is_null()) return std::nullopt;
    vhsdecode_cpp::DropoutInfo d{};
    if (j.contains("fieldLine")) d.field_line = j.at("fieldLine").get<std::vector<int>>();
    if (j.contains("startx")) d.startx = j.at("startx").get<std::vector<int>>();
    if (j.contains("endx")) d.endx = j.at("endx").get<std::vector<int>>();
    return d;
}

json emit_output_json(const vhsdecode_cpp::MetadataOutput& out) {
    json j;
    j["isFirstField"] = out.is_first_field;
    j["detectedFirstField"] = out.detected_first_field;
    j["isDuplicateField"] = out.is_duplicate_field;
    j["syncConf"] = out.sync_conf;
    j["seqNo"] = out.seq_no;
    j["diskLoc"] = out.disk_loc;
    j["fileLoc"] = out.file_loc;
    j["fieldPhaseID"] = out.field_phase_id;
    if (out.dropouts.has_value()) {
        j["dropOuts"] = {
            {"fieldLine", out.dropouts->field_line},
            {"startx", out.dropouts->startx},
            {"endx", out.dropouts->endx},
        };
    }
    j["vitsMetrics"] = json::parse(out.vits_metrics_json);
    if (out.decode_faults.has_value()) j["decodeFaults"] = *out.decode_faults;
    return j;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: vhsdecode_k5_metadata_parity <parity_dir>\n";
        return 2;
    }
    const fs::path dir = argv[1];
    std::ifstream in(dir / "metadata_cases.json");
    if (!in.is_open()) throw std::runtime_error("failed to open metadata_cases.json");
    json root;
    in >> root;

    vhsdecode_cpp::MetadataState state{};
    state.field_order_action = root.at("field_order_action").get<std::string>();
    state.duplicate_prev_field = root.at("duplicate_prev_field_initial").get<bool>();
    state.typec_mode = root.at("typec_mode").get<bool>();

    json out_rows = json::array();
    for (const auto& row : root.at("cases")) {
        const auto& jin = row.at("input");
        vhsdecode_cpp::MetadataInput mi{};
        mi.is_first_field = jin.at("isFirstField").get<bool>();
        mi.detected_first_field = jin.at("detectedFirstField").get<bool>();
        mi.sync_conf = jin.at("syncConf").get<int>();
        mi.seq_no = jin.at("seqNo").get<int>();
        mi.disk_loc = jin.at("diskLoc").get<double>();
        mi.file_loc = jin.at("fileLoc").get<int>();
        if (!jin.at("fieldPhaseID").is_null()) mi.field_phase_id = jin.at("fieldPhaseID").get<int>();
        mi.dropouts = parse_dropouts(jin.at("dropOuts"));
        mi.vits_metrics_json = jin.at("vitsMetrics").dump();

        const auto out = vhsdecode_cpp::build_metadata(mi, state);
        out_rows.push_back({
            {"output", emit_output_json(out)},
            {"duplicateField", out.is_duplicate_field},
            {"writeField", out.write_field},
        });
        if (out.write_field) state.fieldinfo.push_back(out);
    }

    std::ofstream out(dir / "cpp_metadata_cases.json");
    out << out_rows.dump(2);
    return 0;
}

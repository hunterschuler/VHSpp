#include "vhsdecode_cpp/readfield_control_core.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc != 2) {
        throw std::runtime_error("usage: vhsdecode_k7_control_parity <parity_dir>");
    }

    const fs::path dir = argv[1];
    std::ifstream in(dir / "control_cases.json");
    if (!in.is_open()) {
        throw std::runtime_error("failed to open control_cases.json");
    }
    json root;
    in >> root;

    json out = json::array();
    for (const auto& row : root.at("cases")) {
        vhsdecode_cpp::ReadfieldControlInput c{};
        c.field_present = row.at("field_present").get<bool>();
        c.field_valid = row.at("field_valid").get<bool>();
        c.needrerun = row.at("needrerun").get<bool>();
        c.use_agc = row.at("use_agc").get<bool>();
        c.is_first_field = row.at("is_first_field").get<bool>();
        c.sync_confidence = row.at("sync_confidence").get<double>();
        c.fields_written = row.at("fields_written").get<int>();
        c.fdoffset = row.at("fdoffset").get<int>();
        c.offset = row.at("offset").get<int>();
        c.offset_is_none = row.at("offset_is_none").get<bool>();
        c.adjusted = row.at("adjusted").get<bool>();
        c.sync_hz = row.at("sync_hz").get<double>();
        c.ire0_hz = row.at("ire0_hz").get<double>();
        c.ire100_hz = row.at("ire100_hz").get<double>();
        c.actualwhite_ire = row.at("actualwhite_ire").get<double>();
        c.hz_to_ire_sync = row.at("hz_to_ire_sync").get<double>();
        c.hz_to_ire_ire0 = row.at("hz_to_ire_ire0").get<double>();
        c.hz_to_ire_ire100 = row.at("hz_to_ire_ire100").get<double>();
        c.current_vsync_ire = row.at("current_vsync_ire").get<double>();

        const auto r = vhsdecode_cpp::evaluate_readfield_control(c);
        out.push_back({
            {"next_fdoffset", r.next_fdoffset},
            {"adjusted", r.adjusted},
            {"redo", r.redo},
            {"redo_target", r.redo_target},
            {"flush_demod", r.flush_demod},
            {"insert_none", r.insert_none},
            {"insert_field", r.insert_field},
            {"done", r.done},
            {"agc_updated", r.agc_updated},
            {"updated_ire0_hz", r.updated_ire0_hz},
            {"updated_hz_ire", r.updated_hz_ire},
            {"updated_vsync_ire", r.updated_vsync_ire},
        });
    }

    std::ofstream fout(dir / "cpp_control_result.json");
    fout << out.dump(2);
    return 0;
}

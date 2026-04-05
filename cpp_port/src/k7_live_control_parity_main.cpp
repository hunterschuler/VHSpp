#include "vhsdecode_cpp/readfield_control_core.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

int main(int argc, char** argv) {
    if (argc != 2) {
        throw std::runtime_error("usage: vhsdecode_k7_live_control_parity <parity_dir>");
    }

    const fs::path dir = argv[1];
    std::ifstream in(dir / "control_live_cases.json");
    if (!in.is_open()) throw std::runtime_error("failed to open control_live_cases.json");
    json root;
    in >> root;

    json out = json::array();
    for (const auto& row : root.at("cases")) {
        const auto& src = row.at("input");
        vhsdecode_cpp::ReadfieldControlInput c{};
        c.field_present = src.at("field_present").get<bool>();
        c.field_valid = src.at("field_valid").get<bool>();
        c.needrerun = src.at("needrerun").get<bool>();
        c.use_agc = src.at("use_agc").get<bool>();
        c.is_first_field = src.at("is_first_field").get<bool>();
        c.sync_confidence = src.at("sync_confidence").get<double>();
        c.fields_written = src.at("fields_written").get<int>();
        c.fdoffset = src.at("fdoffset").get<int>();
        c.offset = src.at("offset").get<int>();
        c.offset_is_none = src.at("offset_is_none").get<bool>();
        c.adjusted = src.at("adjusted").get<bool>();
        c.sync_hz = src.at("sync_hz").get<double>();
        c.ire0_hz = src.at("ire0_hz").get<double>();
        c.ire100_hz = src.at("ire100_hz").get<double>();
        c.actualwhite_ire = src.at("actualwhite_ire").get<double>();
        c.hz_to_ire_sync = src.at("hz_to_ire_sync").get<double>();
        c.hz_to_ire_ire0 = src.at("hz_to_ire_ire0").get<double>();
        c.hz_to_ire_ire100 = src.at("hz_to_ire_ire100").get<double>();
        c.current_vsync_ire = src.at("current_vsync_ire").get<double>();

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

    std::ofstream fout(dir / "cpp_control_live_result.json");
    fout << out.dump(2);
    return 0;
}

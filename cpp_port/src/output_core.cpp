#include "vhsdecode_cpp/output_core.h"

namespace vhsdecode_cpp {

void write_field_dataset(const WriteoutInput& input, WriteoutState& state) {
    auto fi = input.field_metadata;
    fi.erase("audioSamples");
    state.fieldinfo.push_back(fi);
    state.video_bytes_written += input.picturey_bytes;
    if (state.write_chroma) {
        state.chroma_bytes_written += input.picturec_bytes;
    }
    state.fields_written += 1;
}

nlohmann::json build_vhs_json(const BuildJsonInput& input, const WriteoutState& state) {
    nlohmann::json jout;
    jout["pcmAudioParameters"] = {
        {"bits", 16},
        {"isLittleEndian", true},
        {"isSigned", true},
        {"sampleRate", input.analog_audio},
    };

    nlohmann::json vp;
    vp["numberOfSequentialFields"] = state.fieldinfo.size();
    vp["osInfo"] = input.os_info;
    vp["gitBranch"] = input.git_branch;
    vp["gitCommit"] = input.git_commit;
    vp["system"] = input.system;
    vp["fieldWidth"] = input.field_width;
    vp["sampleRate"] = input.sample_rate;
    vp["black16bIre"] = input.black16b_ire;
    vp["white16bIre"] = input.white16b_ire;
    vp["fieldHeight"] = input.field_height;
    vp["colourBurstStart"] = input.colour_burst_start;
    vp["colourBurstEnd"] = input.colour_burst_end;
    vp["activeVideoStart"] = input.active_video_start;
    vp["activeVideoEnd"] = input.active_video_end;

    if (input.color_system == "MPAL" || input.color_system == "NLINHA") {
        vp["system"] = "PAL-M";
    }

    vp["black16bIre"] = input.black16b_ire * (1.0 - input.level_adjust);
    vp["white16bIre"] = input.white16b_ire * (1.0 + input.level_adjust);
    vp["tapeFormat"] = input.tape_format;
    jout["videoParameters"] = std::move(vp);
    return jout;
}

}  // namespace vhsdecode_cpp

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "vhsdecode_cpp/chroma_afc.h"
#include "vhsdecode_cpp/chroma_phase.h"
#include "vhsdecode_cpp/demod_block.h"

namespace vhsdecode_cpp {

struct ChromaProcessInput {
    std::vector<double> chroma;
    std::vector<std::vector<double>> chroma_heterodyne;
    const std::vector<std::vector<double>>* chroma_heterodyne_ref = nullptr;
    std::vector<PhaseSequenceEntry> phase_sequence;
    vhsdecode::cppport::SosFilter fchroma_final;
    std::optional<vhsdecode::cppport::SosFilter> cafc_chroma_bandpass;
    std::optional<vhsdecode::cppport::IirFilter> fvideo_notch;
    std::optional<vhsdecode::cppport::IirFilter> chroma_audio_notch;
    std::optional<vhsdecode::cppport::IirFilter> chroma_deemphasis;
    vhsdecode::cppport::ChromaAfc* chroma_afc = nullptr;
    int lineoffset = 0;
    int linesout = 0;
    int outwidth = 0;
    int burst_start = 0;
    int burst_end = 0;
    int cafc_move = 0;
    bool is_ntsc = false;
    bool do_cafc = false;
    bool disable_deemph = false;
    bool disable_comb = false;
    bool disable_tracking_cafc = false;
    bool disable_phase_correction = false;
    bool do_chroma_deemphasis = false;
    bool enable_video_notch = false;
    bool keep_intermediates = false;
    std::optional<double> burst_phase_avg;
    double burst_abs_ref = 0.0;
};

struct ChromaProcessResult {
    std::vector<double> chroma_after_cafc_prefilter;
    std::vector<double> chroma_after_burst_deemph;
    std::vector<double> uphet_raw;
    std::vector<double> uphet_phase_comp;
    std::vector<double> uphet_filtered;
    std::vector<double> uphet_after_chroma_deemph;
    std::vector<double> uphet_comb;
    std::vector<double> uphet_final;
    std::vector<std::uint16_t> chroma_u16;
    double mean_rms = 0.0;
    std::optional<vhsdecode::cppport::ChromaAfcMeasurement> cafc_measurement;
    struct PerfStats {
        double cafc_prefilter_s = 0.0;
        double burst_deemph_s = 0.0;
        double upconvert_s = 0.0;
        double phase_comp_s = 0.0;
        double final_filter_s = 0.0;
        double chroma_deemph_s = 0.0;
        double comb_s = 0.0;
        double acc_s = 0.0;
        double to_u16_s = 0.0;
    } perf;
};

// Literal-first C++ transliteration of the exercised process_chroma()/decode_chroma
// path from vhsdecode/chroma.py for NTSC color-under fields.
//
// DIVERGENCE NOTES:
// - Python uses scipy.signal.hilbert plus complex phase rotation in
//   ntsc_phase_comp(); the C++ port mirrors that with FFT-based analytic signal
//   construction and equivalent rotation math.
// - Python uses numba-jitted loops for acc/comb/burst_deemphasis. The C++ port
//   uses direct loops with the same formulas.
//
// !!! LOUD FUTURE-WORK MARKER !!!
// Upstream vhs-decode supports PAL / PAL-M / MPAL / NLINHA / MESECAM / SECAM /
// 405 / 819 and multiple tape families beyond NTSC VHS. This file now contains
// the optional CAFC and chroma-deemphasis branches too, but only the NTSC VHS
// lane is currently fixture-validated in this C++ port. Revisit the upstream
// format inventory in LOGBOOK.md before treating other systems as covered.
ChromaProcessResult process_chroma(const ChromaProcessInput& input);

}  // namespace vhsdecode_cpp

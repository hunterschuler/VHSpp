#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "vhsdecode_cpp/demod_block.h"

namespace vhsdecode_cpp {

struct PhaseSequenceEntry {
    int line_number = 0;
    int current_phase = 0;
    double burst_phase = 0.0;
    double burst_magnitude = 0.0;
    double burst_i = 0.0;
    double burst_q = 0.0;
};

struct ChromaPhaseInput {
    std::vector<double> chroma;
    std::vector<std::vector<double>> chroma_heterodyne;
    vhsdecode::cppport::SosFilter chroma_filter;
    std::optional<std::vector<int>> chroma_rotation;
    std::optional<int> chroma_rotation_index;
    int lineoffset = 0;
    int linesout = 0;
    int outwidth = 0;
    int burst_start = 0;
    int burst_end = 0;
    std::vector<double> burst_sin;
    std::vector<double> burst_cos;
    bool detect_chroma_track_phase = false;
    int rotation_check_start_line = 0;
    bool is_ntsc = false;
};

struct ChromaPhaseResult {
    int track_phase = 0;
    std::vector<PhaseSequenceEntry> phase_sequence;
    std::optional<double> burst_phase_avg;
    std::optional<bool> burst_detected;
};

// Literal-first C++ transliteration of the phase/track portion of
// vhsdecode.chroma:
// - _demod_burst
// - _get_upconverted_burst
// - _get_phase_sequence
// - get_phase_rotation_sequence
//
// DIVERGENCE NOTES:
// - Python uses `sosfiltfilt_rust(...)`; the C++ port uses the same local
//   SciPy-style SOS filtfilt implementation already used in K1.
// - Python stores `None` for some optional values. The C++ port mirrors that
//   with std::optional.
//
// !!! LOUD FUTURE-WORK MARKER !!!
// The upstream phase/track logic also covers PAL / MPAL / NLINHA / MESECAM and
// other tape families with different chroma rotation semantics. The current
// parity proof is for the exercised NTSC VHS path first. Come back here with
// dedicated fixtures before claiming other systems are validated.
ChromaPhaseResult get_phase_rotation_sequence(const ChromaPhaseInput& input);

std::vector<double> upconvert_chroma(const std::vector<double>& chroma,
                                     int lineoffset,
                                     int outwidth,
                                     const std::vector<std::vector<double>>& chroma_heterodyne,
                                     const std::vector<PhaseSequenceEntry>& phase_rotation_sequence);

}  // namespace vhsdecode_cpp

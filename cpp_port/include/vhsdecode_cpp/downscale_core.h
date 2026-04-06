#pragma once

#include <cstdint>
#include <vector>

#include "vhsdecode_cpp/chroma_process.h"

namespace vhsdecode_cpp {

struct DownscaleCoreInput {
    std::vector<float> demod;
    std::vector<double> linelocs;
    std::vector<double> spline_knots;
    std::vector<double> spline_coeffs;
    int lineoffset = 0;
    int outlinecount = 0;
    int outlinelen = 0;
    double inlinelen = 0.0;
    double wow_level_adjust_smoothing = 0.0;
    double y_comb_limit = 0.0;
    bool final_output = false;
    bool export_raw_tbc = false;
    double ire0 = 0.0;
    double hz_ire = 0.0;
    double output_zero = 0.0;
    double vsync_ire = 0.0;
    double out_scale = 0.0;
    int wow_spline_degree = 1;
    bool wow_spline_precomputed = false;
    bool keep_debug_arrays = false;
};

struct DownscaleCoreResult {
    std::vector<float> dsout_float;
    std::vector<std::uint16_t> dsout_u16;
    std::vector<double> interpolated_pixel_locs;
    std::vector<double> wowfactors;
};

// Literal-first C++ transliteration of the exercised luma side of:
// - lddecode.core.Field.computewow_scaled()
// - lddecode.utils.scale_field()
// - vhsdecode.field.FieldShared.downscale()
// - vhsdecode.field.FieldShared.hz_to_output()
//
// !!! LOUD FUTURE-WORK MARKER !!!
// Upstream lets the user explicitly choose linear / quadratic / cubic wow
// interpolation. This port now supports those manual modes too. We should come
// back later and investigate whether automatic per-field selection can improve
// robustness, but that is a later policy layer and must not be conflated with
// the literal-port parity work here.
DownscaleCoreResult downscale_luma(const DownscaleCoreInput& input);

struct NtscVhsDownscaleInput {
    DownscaleCoreInput luma;
    ChromaProcessInput chroma;
    bool write_chroma = true;
};

struct NtscVhsDownscaleResult {
    DownscaleCoreResult luma;
    ChromaProcessResult chroma;
};

// Literal-first integration wrapper for the exercised NTSC VHS path:
// - vhsdecode.field.FieldNTSCVHS.downscale()
// which combines FieldShared downscale() luma output with decode_chroma().
//
// !!! LOUD FUTURE-WORK MARKER !!!
// This wrapper is only intended to validate the exercised NTSC VHS field output
// contract. Other field classes (Betamax, U-Matic, PAL variants, 405, 819,
// etc.) must get their own fixture-backed integration passes later.
NtscVhsDownscaleResult downscale_ntsc_vhs(const NtscVhsDownscaleInput& input);

}  // namespace vhsdecode_cpp

# LOGBOOK

## 2026-04-02 - K1 Port Status

This repository was reset from the old VHSpp codebase to the latest upstream `vhs-decode` source tree, then a new C++ port subtree was started under [`cpp_port/`](/media/hunter/DATA/GitHub/VHSpp/cpp_port).

The current validated milestone is:

- K1 (`VHSDecode.demodblock(...)`) is ported to C++ with very tight numeric parity against Python `vhs-decode`.
- The remaining work has moved upstream to later pipeline stages; K1 is no longer the major blocker.

## Repository / Logistics Changes

### Tree reset

- The previous VHSpp implementation was wiped from the working tree.
- The latest `vhs-decode` was pulled into [`/media/hunter/DATA/GitHub/vhs-decode`](/media/hunter/DATA/GitHub/vhs-decode), then copied into this repo as the new baseline.
- The old VHSpp git history was preserved underneath the new imported tree.

### Native/system dependencies

- `fftw3` is used directly from C++ for FFT work in the K1 port.
- `libsoxr-dev` is installed system-wide and the C++ port calls the native `soxr` C API directly.
  - This replaced an earlier temporary workaround that extracted `soxr.h` locally.
  - Current state is clean: standard native C/C++ linkage, no Python wrapper involved.

### Python comparison environment

- The conda environment `vhs-decode` was activated and used for parity dumping/comparison.
- Python remained the ground truth for all parity checks.

## New C++ Port Subtree

Main new files:

- [cpp_port/include/vhsdecode_cpp/demod_block.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/demod_block.h)
- [cpp_port/src/demod_block.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/demod_block.cpp)
- [cpp_port/include/vhsdecode_cpp/k1_context.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/k1_context.h)
- [cpp_port/src/k1_context.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k1_context.cpp)
- [cpp_port/include/vhsdecode_cpp/chroma_afc.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/chroma_afc.h)
- [cpp_port/src/chroma_afc.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/chroma_afc.cpp)
- [cpp_port/src/k1_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k1_parity_main.cpp)
- [cpp_port/tools/dump_k1_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k1_python.py)
- [cpp_port/tools/compare_k1_outputs.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k1_outputs.py)

Build integration:

- [cpp_port/CMakeLists.txt](/media/hunter/DATA/GitHub/VHSpp/cpp_port/CMakeLists.txt)
- [CMakeLists.txt](/media/hunter/DATA/GitHub/VHSpp/CMakeLists.txt)

## What Was Ported

### 1. Full K1 runtime path

Ported into [demod_block.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/demod_block.cpp):

- RF notch application
- `RFVideo` multiplication
- Hilbert-envelope path
- `FEnvPost`
- weak-signal RF high boost
- analytic-signal Hilbert FM demod
- diff-demod spike replacement
- `VideoEQ`
- chroma trap
- main `FVideo` path
- nonlinear deemphasis
- sub-deemphasis
- FSC notch
- `FVideo05`
- chroma source extraction (`demod_burst`)

### 2. Full K1 filter/context construction

Ported into [k1_context.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k1_context.cpp):

- `RFVideo`
- `hilbert`
- `RFTop`
- `FVideo`
- `FVideo05`
- `FEnvPost`
- `NLAmplitudeLPF`
- `NLHighPassF`
- `FVideoNotch`
- `FVideoNotchF`
- `FChromaAudioNotch`
- `fsc_notch`
- `FVideoBurst`
- `VideoEQ` config
- `ChromaTrap` config
- sub-deemphasis config
- runtime `DemodBlockOptions`

### 3. `ChromaAFC`

Ported into [chroma_afc.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/chroma_afc.cpp):

- carrier measurement / retuning
- `cc_phase`
- heterodyne wave generation
- FSC wave generation
- narrowband tracking / drift logic
- `freqOffset(...)`
- `getChromaBandpass()`
- `getChromaBandpassFinal()`
- `getChromaHet()`
- `getFSCWaves()`
- `resetCC()`

### 4. Native `soxr`

The C++ chroma trap now uses the real native `soxr` library directly in [demod_block.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/demod_block.cpp).

This closes an earlier parity gap where a temporary linear-resampler fallback was used.

## Required Divergences

All required divergences are called out inline with `DIVERGENCE` comments in the C++ files. The important ones are listed here.

### 1. Rust helpers replaced with native C++ implementations

Python `vhs-decode` uses Rust helpers such as:

- `vhsd_rust.unwrap_hilbert`
- `sosfiltfilt_rust`

The C++ port cannot call those directly, so:

- Hilbert unwrap is implemented locally in [demod_block.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/demod_block.cpp)
- `sosfiltfilt` is implemented locally in [demod_block.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/demod_block.cpp)

This is a divergence in implementation source, not currently a large divergence in numeric behavior.

### 2. SciPy internals are mirrored, not embedded

The port does not embed SciPy. Instead it reproduces the necessary behavior in C++:

- `butter`
- `buttord`
- `freqz`
- `sosfreqz`
- `firwin`
- `iirnotch`

These were initially approximate and then tightened until the K1 filters matched extremely closely.

### 3. `soxr`

Python reaches `soxr` through the Python package.

C++ now reaches the same native library directly through the C API.

This is a packaging / call-surface divergence, not a meaningful DSP divergence.

### 4. `zpk_to_sos` pairing logic

The remaining non-literal part of the K1 filter builder is that SciPy’s internal SOS pairing is not imported verbatim. A local C++ pairing strategy is used in [chroma_afc.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/chroma_afc.cpp).

For the validated K1 paths, the resulting SOS coefficients now match Python where needed, including `FVideoBurst`.

## Precision / Parity Harness

Parity tooling:

- Python dumper: [dump_k1_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k1_python.py)
- C++ runner: [k1_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k1_parity_main.cpp)
- Comparator: [compare_k1_outputs.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k1_outputs.py)

Working parity artifacts:

- [/media/hunter/DATA/captures/k1_parity](/media/hunter/DATA/captures/k1_parity)

Compared outputs:

- `envelope`
- `demod`
- `demod_05`
- `demod_burst`

Compared filter products:

- `RFVideo`
- `hilbert`
- `FVideo`
- `FVideo05`
- `RF_bpf`
- `RF_lpf_extra`
- `RF_hpf_extra`
- `RF_peak`
- `RF_audio`
- `RF_ramp`
- `deemp`
- `video_lpf`
- `custom`
- `filter_05`
- `FEnvPost`
- `FVideoBurst`

## Final K1 Precision Results

### Signal-output parity

Current measured output differences on the same real RF block:

- `envelope`
  - diff RMS: `0.0932682161`
  - signal RMS: `1460.9176336`
  - relative RMS error: `0.0063842214%`

- `demod`
  - diff RMS: `0.1557184737`
  - signal RMS: `2808481.9374888`
  - relative RMS error: `0.0000055446%`

- `demod_05`
  - diff RMS: `0.0317210948`
  - signal RMS: `2808061.4200849`
  - relative RMS error: `0.0000011296%`

- `demod_burst`
  - diff RMS: `0.1284012111`
  - signal RMS: `1543.7230202`
  - relative RMS error: `0.0083176327%`

### Interpretation

- `demod` and `demod_05` are effectively at parity.
- `envelope` is tight enough that the remaining difference is small in practical terms.
- `demod_burst` is no longer “far off”; it is now in the same general precision class as the rest of K1.

### Filter parity highlights

Current filter diffs:

- `filter_RFVideo`
  - max abs: `1.8948788e-05`
  - mean abs: `4.4421568e-06`

- `filter_hilbert`
  - exact

- `filter_FVideo`
  - max abs: `2.03535442e-13`

- `filter_FVideo05`
  - max abs: `2.02833219e-13`

- `filter_RF_peak`
  - max abs: `1.99840144e-15`

- `filter_05`
  - max abs: `6.75322301e-16`

- `filter_FEnvPost_flat`
  - max abs: `1.38777878e-17`

- `filter_FVideoBurst_flat`
  - max abs: `6.66133815e-16`

## Important Debugging Findings

### 1. `RFVideo` discrepancy was real and unacceptable

Earlier in the port, `RFVideo` differed enough to block trust in K1.

This was fixed by replacing rough local filter approximations with a real digital ZPK/bilinear construction in [k1_context.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k1_context.cpp).

### 2. `FVideo05` discrepancy was caused by a literal FIR bug

The 0.5 MHz FIR path was wrong because the C++ sinc formula was missing a factor in the argument.

After fixing that, `filter_05` and `FVideo05` matched to near machine precision.

### 3. Chroma mismatch turned out not to be `ChromaAFC` itself

The remaining large chroma mismatch was traced through:

- `FVideoBurst` approximation
- SOS section ordering
- and finally a real `sosfiltfilt` bug

The last major bug was:

- the C++ `sosfiltfilt` implementation was re-scaling each SOS section from the already-filtered endpoint
- SciPy uses the fixed pass endpoint for the whole forward pass and for the whole backward pass

Fixing that collapsed the chroma mismatch from “far off” to the current small residual.

## Current Bottom Line

K1 is now close enough to `vhs-decode` numerically that it should no longer be treated as the main suspect for remaining decode quality differences.

The current practical conclusion is:

- K1 luma path: very high parity
- K1 sync-path branch: very high parity
- K1 chroma-source path: very high parity

That moves the remaining porting focus downstream to later stages rather than continuing to churn inside K1.

## Recommended Next Step

Proceed to K2 / sync / line-location stages with the same parity-harness discipline used for K1:

1. port the stage
2. dump Python outputs
3. dump C++ outputs
4. identify the first true divergence
5. close it numerically before moving on

## K2 Completion Update

K2 is now in near-perfect parity with `vhs-decode`.

### What was ported

The K2 port now includes:

- pulse-finding front-end from `vhsdecode/addons/resync.py`
  - [resync_core.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/resync_core.h)
  - [resync_core.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/resync_core.cpp)
- serration / vsync front-end from `vhsdecode/addons/vsyncserration.py`
  - [vsync_serration.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/vsync_serration.h)
  - [vsync_serration.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/vsync_serration.cpp)
- runtime `get_pulses()` / level-check path
  - [resync_runtime.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/resync_runtime.h)
  - [resync_runtime.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/resync_runtime.cpp)
- field-side sync / line-location logic from `sync.pyx` and `field.py`
  - [sync_core.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/sync_core.h)
  - [sync_core.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/sync_core.cpp)

That includes:

- `get_first_hsync_loc(...)`
- `valid_pulses_to_linelocs(...)`
- `refine_linelocs_hsync(...)`
- `pulse_qualitycheck(...)`
- vblank state machine
- `refine_pulses(...)`

### Harnesses used

The K2 parity work used both staged and full-chain harnesses:

- staged pulse parity
  - [dump_k2_get_pulses_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k2_get_pulses_python.py)
  - [k2_get_pulses_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k2_get_pulses_parity_main.cpp)
  - [compare_k2_get_pulses.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k2_get_pulses.py)
- staged serration parity
  - [dump_k2_vsync_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k2_vsync_python.py)
  - [k2_vsync_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k2_vsync_parity_main.cpp)
  - [compare_k2_vsync.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k2_vsync.py)
- staged refine-pulse parity
  - [dump_k2_refine_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k2_refine_python.py)
  - [k2_refine_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k2_refine_parity_main.cpp)
  - [compare_k2_refine.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k2_refine.py)
- full sync-chain parity
  - [dump_k2_metadata_hit.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k2_metadata_hit.py)
  - [k2_sync_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k2_sync_parity_main.cpp)
  - [compare_k2_outputs.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k2_outputs.py)

### Key logistical fix

The most important K2 fixture bug was not in the C++ sync code at all. It was in the Python-side parity tooling.

The real `vhs-decode` CLI uses:

- `loader_input_freq = 28.636363636363637` for CXADC input
- but instantiates `VHSDecode(..., inputfreq=40)` unless `--no_resample` is used

The parity scripts had incorrectly been instantiating `VHSDecode(..., inputfreq=28.636...)` directly. That bypassed the normal resample path and produced misleading “all failure-path, no success-path fields” results.

After aligning the fixture scripts with the actual CLI behavior, real K2 success-path fields appeared immediately.

### Required divergences

The same general K1 rule still applies: where Python used Cython or Rust helpers, the C++ port mirrors behavior directly in native code instead of calling the Python implementation.

Important K2 parity-preserving choices include:

- preserving bug-for-bug behavior where required for fidelity
  - for example, the Python-side `c_median` behavior preserved in the sync path
- preserving the exact HSYNC right-edge bias constant used by `sync.pyx`
- using the same resampled `40 MHz` decoder path as the real CLI for all live K2 fixtures

No major “this is only approximate” divergence remains in the core K2 math path.

### Precision results

#### Staged parity

- `get_pulses()` on a non-empty real field:
  - exact match
  - `python_count = 2`
  - `cpp_count = 2`
  - `start_mismatch_count = 0`
  - `len_mismatch_count = 0`
- `get_first_hsync_loc(...)` synthetic fuzz:
  - exact across `1000` cases
  - `first_hsync_max_abs = 0.0`
  - `line0loc_max_abs = 0.0`
  - `next_field_max_abs = 0.0`
- `valid_pulses_to_linelocs(...)` synthetic fuzz:
  - exact across `1000` cases
  - `linelocs0_max_abs = 0.0`
  - `lineloc_errs_total_mismatch = 0`
- `refine_pulses(...)` synthetic fuzz:
  - exact across `1000` cases
  - `count_equal_all = true`
  - `start_mismatch_total = 0`
  - `type_mismatch_total = 0`
  - `valid_mismatch_total = 0`
- `refine_linelocs_hsync(...)` synthetic fuzz:
  - effectively exact across `1000` cases
  - `linebad_total_mismatch = 0`
  - `linelocs2_max_abs = 1.9514118321239948e-08`
  - `linelocs2_rms_max = 1.935356380561731e-08`
- serration front-end on field-173 substrate:
  - control-flow matches
  - envelope correlation `0.9999992420458891`

#### Full live-field parity

Two real success-path live fields from `/media/hunter/DATA/captures/TAPE_1_1min_33pct.u8` now compare exactly across the full K2 sync chain:

- [k2_parity_1_live](/media/hunter/DATA/captures/k2_parity_1_live)
- [k2_parity_900_live](/media/hunter/DATA/captures/k2_parity_900_live)

For both fields:

- `first_hsync_loc_abs = 0.0`
- `line0loc_abs = 0.0`
- `hsync_start_line_abs = 0.0`
- `next_field_abs = 0.0`
- `prev_hsync_diff_abs = 0.0`
- `first_field_match = true`
- `progressive_field_match = true`
- `vblank_pulses_equal = true`
- `linelocs0.max_abs = 0.0`
- `linelocs0.rms = 0.0`
- `lineloc_errs.mismatch_count = 0`
- `linelocs2.max_abs = 0.0`
- `linelocs2.rms = 0.0`
- `linebad.mismatch_count = 0`

### Current bottom line

K2 is now effectively done at the parity level:

- staged K2 algorithms are exact or at floating-point-noise level
- the full live K2 sync chain matches exactly on rich success-path real fields
- the remaining port focus should move downstream to the next stage rather than continuing to churn inside K2

### Additional 40 MHz confirmation

After K2 was already exact on the longer native-rate `TAPE_1` fixture, a second confirmation pass was done on a true `40 MHz` archive-derived raw chunk:

- source chunk:
  - [wcbs-tv-1984-11-04_40msps_15m_20m.u8](/media/hunter/DATA/captures/wcbs-tv-1984-11-04_40msps_15m_20m.u8)
- CLI metadata generated from that same chunk:
  - [wcbs_40m_15m_20m_vhsdecode_test.tbc.json](/media/hunter/DATA/captures/wcbs_40m_15m_20m_vhsdecode_test.tbc.json)
- targeted live parity fixture from the chunk:
  - [k2_parity_40m_15m_field1](/media/hunter/DATA/captures/k2_parity_40m_15m_field1)

This required two important fixture corrections:

1. The parity scripts had to follow the real CLI resample workflow:
   - source / loader rate = actual capture rate
   - decoder internal rate = `40 MHz`
2. The archive sample is `NTSC VHS EP`, so the parity tools had to pass:
   - `rf_options["tape_speed"] = "ep"`

Once those two fixture issues were corrected, the full live K2 sync parity on the `40 MHz` EP chunk also matched exactly:

- `first_hsync_loc_abs = 0.0`
- `line0loc_abs = 0.0`
- `hsync_start_line_abs = 0.0`
- `next_field_abs = 0.0`
- `prev_hsync_diff_abs = 0.0`
- `vblank_pulses_equal = true`
- `linelocs0.max_abs = 0.0`
- `linelocs2.max_abs = 0.0`
- `linebad.mismatch_count = 0`

This means K2 now has exact live-field confirmation on:

- a longer native-rate fixture
- and a real `40 MHz` fixture

That removes the remaining ambiguity around whether K2 parity only held on one input style.

## 2026-04-02 - K3 phase and chroma finish parity

### Scope

K3 was defined as the chroma stage immediately downstream of K2 for the active NTSC VHS path.

The first K3 checkpoint was the phase/track portion of `vhsdecode/chroma.py`:

- `_demod_burst`
- `_get_upconverted_burst`
- `_get_phase_sequence`
- `get_phase_rotation_sequence`
- `upconvert_chroma`

The second K3 checkpoint was the exercised `process_chroma()` / `decode_chroma()` finish path:

- `burst_deemphasis`
- `upconvert_chroma`
- `ntsc_phase_comp`
- `FChromaFinal` SOS filtering
- `comb_c_ntsc`
- `acc`
- `chroma_to_u16`

### New C++ files

- [cpp_port/include/vhsdecode_cpp/chroma_phase.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/chroma_phase.h)
- [cpp_port/src/chroma_phase.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/chroma_phase.cpp)
- [cpp_port/include/vhsdecode_cpp/chroma_process.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/chroma_process.h)
- [cpp_port/src/chroma_process.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/chroma_process.cpp)
- [cpp_port/src/k3_phase_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k3_phase_parity_main.cpp)
- [cpp_port/src/k3_process_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k3_process_parity_main.cpp)
- [cpp_port/tools/dump_k3_phase_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k3_phase_python.py)
- [cpp_port/tools/compare_k3_phase.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k3_phase.py)
- [cpp_port/tools/dump_k3_process_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k3_process_python.py)
- [cpp_port/tools/compare_k3_process.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k3_process.py)

### Important bugs found/fixed during K3

1. Python dumper bug
- The first K3 dumper accidentally fed the full tuple from `field.downscale(...)` into `upconvert_chroma()` instead of the chroma plane from `ldd.Field.downscale(...)`.
- This was a fixture bug, not a port bug.

2. Negative modulo parity trap
- Python wraps negative phases with `% 4` into `0..3`.
- C++ negative remainder semantics produced `-1` for the VHS `chroma_rotation = [-1, 1]` path.
- That caused an out-of-bounds heterodyne index and a crash in the first C++ K3 run.
- Fixed by explicitly mirroring Python modulo behavior.

3. NTSC phase compensation bug
- The first C++ `ntsc_phase_comp()` attempt only reconstructed the real half of the analytic signal.
- Python uses SciPy `hilbert()` and rotates the full complex analytic signal before taking the real part.
- Replaced the broken helper with a proper FFT-domain analytic-signal construction.

4. `chroma_to_u16()` semantics
- Raw NumPy `astype(np.uint16)` was not the right reference behavior here.
- Under the actual `vhsdecode.chroma_to_u16()` numba path, out-of-range values behaved like saturation/clamping on the exercised samples.
- The C++ packer was updated to clamp to `[0, 65535]` before converting.

### K3 phase / track parity

Fixtures:

- native-rate:
  - [k3_phase_native_field900](/media/hunter/DATA/captures/k3_phase_native_field900)
- `40 MHz`:
  - [k3_phase_40m_field1](/media/hunter/DATA/captures/k3_phase_40m_field1)

Measured results:

Common path:
- `track_phase_equal = true`
- `burst_detected_equal = true`
- `phase_sequence` line numbers and chosen heterodyne phases matched exactly

`40 MHz` field:
- `burst_phase_avg_abs = 1.4103117962349643e-07`
- `uphet_max_abs = 0.00039760489016771317`
- `uphet_rms = 2.3446425132780134e-05`
- per-line phase-sequence component diffs:
  - `burst_phase max = 1.4952068340789992e-05`
  - `burst_magnitude max = 0.020325191493611783`
  - `I max = 0.013404520650510676`
  - `Q max = 0.016654115854180418`

Native-rate field:
- `burst_phase_avg_abs = 1.8719404693001707e-07`
- `uphet_max_abs = 0.00047903566155582666`
- `uphet_rms = 2.6280732420158833e-05`
- per-line phase-sequence component diffs:
  - `burst_phase max = 1.989554110082281e-05`
  - `burst_magnitude max = 0.024377546418691054`
  - `I max = 0.019550410186639056`
  - `Q max = 0.01578836106637027`

### Optional detect_chroma_track_phase branch

This branch was explicitly forced on in the dumper and rerun for both fixtures:

- [k3_phase_40m_field1_detect](/media/hunter/DATA/captures/k3_phase_40m_field1_detect)
- [k3_phase_native_field900_detect](/media/hunter/DATA/captures/k3_phase_native_field900_detect)

Results:
- branch executed cleanly on both fixtures
- no track flip was actually triggered on these two fields
- parity remained effectively identical to the non-forced runs

So the branch is exercised and matching, but the particular fields tested do not require a rotation change near the head-switch region.

### Full exercised process_chroma() parity

Fixtures:

- native-rate:
  - [k3_process_native_field900](/media/hunter/DATA/captures/k3_process_native_field900)
- `40 MHz`:
  - [k3_process_40m_field1](/media/hunter/DATA/captures/k3_process_40m_field1)

`40 MHz` process results:
- `chroma_after_burst`: exact
- `uphet_raw rms = 3.991272225552825e-05`
- `uphet_phase_comp rms = 0.00044424170301794253`
- `uphet_filtered rms = 0.00029984475226425475`
- `uphet_comb rms = 0.0002062084756875512`
- `uphet_final rms = 0.05544875386611344`
- `signal_rms = 2619.1399503607067`
- relative final error = `0.0021170596041833146%`
- packed `u16` mismatch:
  - `max_abs = 2`
  - `rms = 0.11270371231937232`

Native-rate process results:
- `chroma_after_burst`: exact
- `uphet_raw rms = 3.9286786247615316e-05`
- `uphet_phase_comp rms = 0.00045172843461879493`
- `uphet_filtered rms = 0.00031504218147254865`
- `uphet_comb rms = 0.0002360067653998795`
- `uphet_final rms = 0.0736529546066008`
- `signal_rms = 2115.4767956937058`
- relative final error = `0.0034816243201783066%`
- packed `u16` mismatch:
  - `max_abs = 2`
  - `rms = 0.2259720896126723`

### Current K3 bottom line

For the actual NTSC VHS path exercised by both fixtures, K3 is now effectively done at the parity level:

- phase / track handling matches on both native-rate and `40 MHz`
- the optional `detect_chroma_track_phase` branch was explicitly exercised and matched on the tested fields
- the full exercised `process_chroma()` / `decode_chroma()` path is now at very low error on both fixtures
- final chroma float output error is around `0.0021%` on the `40 MHz` fixture and `0.0035%` on the native-rate fixture

### Remaining scope note

The current K3 parity coverage is for the exercised NTSC VHS path on these fixtures.

The following optional branches were not needed by either tested fixture and were therefore not the blocking parity target in this pass:

- CAFC prefilter / `freqOffset()` retuning path inside `process_chroma()`
- optional `chroma_deemphasis`
- PAL/other color-system comb paths

Those are now separate follow-on work rather than blockers for the active NTSC VHS K3 lane.

## 2026-04-02 - K3 Branch Completion And Upstream Format Inventory

The remaining K3 stubs have now been fleshed out in code:

- optional CAFC prefilter / `freqOffset()` retuning path inside `process_chroma()`
- optional `chroma_deemphasis`

This matters because those were exactly the kinds of branches that could have smuggled `40 MHz` assumptions back into the port if left unimplemented.

### What CAFC Does

CAFC = chroma automatic frequency control.

What it does in practice:

1. bandpass-filters the TBC’d chroma-under signal in the expected chroma-under region
2. measures the actual carrier center frequency from the tape
3. updates long-term drift / bias state
4. updates the heterodyne wave table used for chroma upconversion

So instead of assuming the chroma-under carrier is exactly where theory says it should be, CAFC lets the decoder follow real tape drift.

### Upstream Format Inventory We Must Not Forget

Upstream `vhs-decode` supports a much broader format surface than the current NTSC VHS fixture lane.

Tape families / formats found in upstream:

- `VHS`
- `VHSHQ`
- `SVHS`
- `SVHS_ET`
- `UMATIC`
- `UMATIC_HI`
- `BETAMAX`
- `BETAMAX_HIFI`
- `SUPERBETA`
- `VIDEO8`
- `HI8`
- `EIAJ`
- `QUADRUPLEX`
- `VCR`
- `VCR_LP`
- `TYPEC`
- `TYPEB`
- `VIDEO2000`
- `VHD`

Systems / color systems found in upstream:

- `NTSC`
- `PAL`
- `PALM`
- `MPAL`
- `NLINHA`
- `MESECAM`
- `SECAM`
- `405`
- `819`

Current fixture-validated status in the C++ port:

- `NTSC VHS`: yes, through K3
- everything else above: not yet fixture-validated in the C++ port

That is why there are now LOUD inline comments in the chroma headers: they are there to stop us from silently forgetting that non-NTSC / non-VHS support still needs dedicated fixtures and parity work later.

### Forced-branch parity result

The forced `40 MHz` branch fixture is:

- [k3_process_40m_field1_forced](/media/hunter/DATA/captures/k3_process_40m_field1_forced)

This explicitly exercises both:

- `do_cafc=True`
- `do_chroma_deemphasis=True`

Measured parity on that forced branch:

- `cafc_measured_hz_abs = 0.41410300706047565`
- `cafc_long_term_offset_hz_abs = 2.6363501325121774e-05`
- `cafc_phase_rad_abs = 0.0`
- `chroma_after_cafc_prefilter rms diff = 0.04569986384301735`
- `uphet_after_chroma_deemph rms diff = 0.01933475314687893`
- `uphet_final rms diff = 0.3358159441352598`
- `uphet_final signal rms = 2366.3439229005658`
- relative final error = `0.014191828957380594%`
- packed `u16` mismatch:
  - `max_abs = 9`
  - `rms = 0.47898641573870027`

So the forced CAFC + chroma-deemphasis lane is now in very strong shape on the true `40 MHz` fixture too.

### Native-rate note for the forced branches

The native-rate forced-branch dumper path on the current `TAPE_1` fixture is still flaky on the Python side: forcing these branches produced field objects that reached `decode_chroma_phase_rotation()` without populated `linelocs`, which is a fixture-extraction problem in the Python walk, not a known C++ parity failure.

So the current status is:

- forced CAFC + chroma-deemphasis: validated on `40 MHz`
- mainline K3 path: validated on both native-rate and `40 MHz`
- native-rate forced-branch fixture extraction: still needs a cleaner Python-side success-path substrate later

## 2026-04-02 - K4 Downscale Native-Rate Unblocked

K4 began as the exercised luma side of `Field.downscale()`:

- `lddecode.core.Field.computewow_scaled()`
- `lddecode.utils.scale_field()`
- `vhsdecode.field.FieldShared.downscale()` luma path
- final `hz_to_output_array()` conversion

The first K4 pass already had strong parity on the true `40 MHz` fixture, but
the native-rate dumper was failing inside Python before comparison because the
metadata-driven `decodefield()` walk could hand back a field object without
final `linelocs`.

### What changed

The K4 Python dumper now has a live `readfield()` extraction mode, matching the
fixture strategy that already worked for the successful K2/K3 native-rate
parity runs.

This matters because it shows the native-rate K4 blocker was fixture
acquisition, not a demonstrated C++ downscale defect.

### Native-rate K4 parity result

Artifact directory:

- [k4_downscale_native_live](/media/hunter/DATA/captures/k4_downscale_native_live)

Measured parity on the native-rate fixture:

- `interpolated_pixel_locs`
  - `max_abs = 2.3283064365386963e-10`
  - `mean_abs = 2.4935116213282722e-11`
  - `rms = 4.9327257148522075e-11`
- `wowfactors`
  - `max_abs = 5.384581669432009e-14`
  - `mean_abs = 1.2350810609930388e-14`
  - `rms = 1.685895189590486e-14`
- `dsout_float`
  - `max_abs = 1.25`
  - `mean_abs = 0.12291710190949735`
  - `rms = 0.21199251020906548`
  - signal RMS = `3835067.3795706956`
  - relative RMS error = `0.00000553%`
- `dsout_u16`
  - `max_abs = 1.0`
  - `mean_abs = 0.49880499728408473`
  - `rms = 0.7062612811729698`

### 40 MHz K4 parity result, now in perspective

Artifact directory:

- [k4_downscale_40m_field1](/media/hunter/DATA/captures/k4_downscale_40m_field1)

Measured parity on the true `40 MHz` fixture:

- `interpolated_pixel_locs`
  - `max_abs = 1.1641532182693481e-10`
  - `mean_abs = 1.988495229282877e-11`
  - `rms = 4.176229920833454e-11`
- `wowfactors`
  - `max_abs = 4.6407322429331543e-14`
  - `mean_abs = 8.938136426281468e-15`
  - `rms = 1.2299147435187164e-14`
- `dsout_float`
  - `max_abs = 1.25`
  - `mean_abs = 0.14962917310826057`
  - `rms = 0.23388108394149065`
  - signal RMS = `3937867.268896578`
  - relative RMS error = `0.00000594%`
- `dsout_u16`
  - `max_abs = 1.0`
  - `mean_abs = 0.4816487694814691`
  - `rms = 0.6940091998536252`

### K4 status

So K4’s first checkpoint is now strong on both lanes:

- native-rate fixture: yes
- true `40 MHz` fixture: yes

The wow/TBC mapping itself is effectively exact, and the full exercised luma
downscale path is now in the same “tiny compared to signal scale” regime as the
earlier K1/K3 parity work.

## 2026-04-02 - K4 Full NTSC VHS Field Output Integration

After the first K4 rung (luma side of `Field.downscale()`) matched cleanly on
both lanes, the next step was the real NTSC VHS field output contract:

- `vhsdecode.field.FieldNTSCVHS.downscale()`

That is the field-level API which returns:

- final luma output
- final chroma output

### Important integration trap that surfaced

The first full-field K4 compare looked badly wrong on chroma while luma stayed
excellent. This turned out not to be a C++ signal-path regression.

The root cause was a field-state contract issue in the Python reference harness:

- `FieldNTSCVHS.downscale()` uses the stored burst-lock state already present on
  the field object
  - `field.phase_sequence`
  - `field.burst_phase_avg`
- my first harness version recomputed `decode_chroma_phase_rotation()` instead
  of using that stored state

That matters because the field-level API is not just “run the chroma math
again”; it is “run chroma decode using the field’s already-processed burst-lock
context.”

So the harness was fixed to mirror the real upstream field contract exactly.

### Full-field K4 artifacts

- native-rate:
  - [k4_field_native_live](/media/hunter/DATA/captures/k4_field_native_live)
- true `40 MHz`:
  - [k4_field_40m_field1](/media/hunter/DATA/captures/k4_field_40m_field1)

### Final full-field K4 parity

Native-rate fixture:

- `field_luma_u16`
  - `max_abs = 1`
  - `mean_abs = 0.49880499728408473`
  - `rms = 0.7062612811729698`
- `field_chroma_u16`
  - `max_abs = 1`
  - `mean_abs = 0.06219028120168805`
  - `rms = 0.24937979309015407`

True `40 MHz` fixture:

- `field_luma_u16`
  - `max_abs = 1`
  - `mean_abs = 0.4816487694814691`
  - `rms = 0.6940091998536252`
- `field_chroma_u16`
  - `max_abs = 6`
  - `mean_abs = 0.01865207036309698`
  - `rms = 0.15980600783049614`

### K4 status

K4 now has two strong checkpoints on both lanes:

1. exercised luma side of `Field.downscale()`
2. full NTSC VHS field output integration path

The important lesson from this stage is that parity work downstream of K3/K4
must preserve upstream field-state contracts, not just the numeric signal
transforms in isolation.

## 2026-04-02 - K4 Quadratic And Cubic Wow Modes

The manual upstream wow interpolation modes are now ported too:

- `linear`
- `quadratic`
- `cubic`

This work was done in the K4 downscale core rather than left as another stump.

### Why this exists

Upstream `lddecode.core.Field.computewow_scaled()` allows explicit user
selection of the interpolation model used for the line-location warp:

- linear
- quadratic
- cubic

These are user-selected policy modes, not hidden auto-magic. For parity work we
therefore need to port the manual behavior first.

### Important future-work note

The *ideal* decoder may eventually choose the safest/most-accurate wow
interpolation mode automatically based on field quality. That is a later policy
layer.

For now, the C++ port intentionally mirrors upstream’s explicit manual modes.
There are LOUD inline comments in the K4 code reminding us to revisit automatic
selection later, after parity work is complete.

### Forced quadratic parity

Native-rate fixture:

- [k4_downscale_native_quadratic](/media/hunter/DATA/captures/k4_downscale_native_quadratic)
- `interpolated_pixel_locs rms = 1.223309536471752e-10`
- `wowfactors rms = 5.533780287784963e-14`
- `dsout_float rms = 0.21332367017958742`
- `dsout_u16 rms = 0.7060157192276946`

True `40 MHz` fixture:

- [k4_downscale_40m_quadratic](/media/hunter/DATA/captures/k4_downscale_40m_quadratic)
- `interpolated_pixel_locs rms = 1.0026724400809934e-10`
- `wowfactors rms = 4.8257162867631103e-14`
- `dsout_float rms = 0.2339765316010868`
- `dsout_u16 rms = 0.6946621245946419`

Quadratic mode is therefore in the same essentially-exact regime as the
original linear K4 rung.

### Forced cubic parity

Native-rate fixture:

- [k4_downscale_native_cubic](/media/hunter/DATA/captures/k4_downscale_native_cubic)
- `interpolated_pixel_locs max_abs = 1.1660216841846704e-05`
- `interpolated_pixel_locs rms = 5.489105536593916e-07`
- `wowfactors max_abs = 2.8540398488985375e-08`
- `wowfactors rms = 7.59701304999081e-10`
- `dsout_float rms = 0.21384148022645424`
- `dsout_u16 rms = 0.706453529123062`

True `40 MHz` fixture:

- [k4_downscale_40m_cubic](/media/hunter/DATA/captures/k4_downscale_40m_cubic)
- `interpolated_pixel_locs max_abs = 4.556481144391e-06`
- `interpolated_pixel_locs rms = 2.0371072224109148e-07`
- `wowfactors max_abs = 1.0206605338680674e-08`
- `wowfactors rms = 2.5939288025294824e-10`
- `dsout_float rms = 0.23337472747255408`
- `dsout_u16 rms = 0.6944124609040768`

Cubic mode is slightly less exact than linear/quadratic in the raw interpolated
pixel-location arrays, but still extremely tight in the final downscaled video
output.

### Implementation note

The C++ side now explicitly builds and evaluates the same spline families used
by the upstream path:

- quadratic not-a-knot
- cubic natural

This is why K4 now supports all three upstream manual wow modes instead of only
the default linear one.

## K5 start: metadata / field-write sequencing

K5 begins at the next downstream layer after K4 field output: the VHS-specific
metadata / write pipeline contract in `vhsdecode.process.VHSDecode`.

For the first K5 checkpoint I treated `buildmetadata()` as the kernel, not the
entire file writer. This is the stateful sequence logic that decides, for each
decoded field:

- `isFirstField`
- `detectedFirstField`
- `isDuplicateField`
- `syncConf`
- `seqNo`
- `diskLoc`
- `fileLoc`
- `fieldPhaseID`
- `decodeFaults`
- whether the field is written, duplicated, or dropped

This is exactly the kind of logic where a "mostly right" port is dangerous, so
the C++ side mirrors upstream literally first and only later will we consider
any smarter policy layer.

### Ported files

- [cpp_port/include/vhsdecode_cpp/metadata_core.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/metadata_core.h)
- [cpp_port/src/metadata_core.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/metadata_core.cpp)
- [cpp_port/src/k5_metadata_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k5_metadata_parity_main.cpp)
- [cpp_port/tools/dump_k5_metadata_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k5_metadata_python.py)
- [cpp_port/tools/compare_k5_metadata.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k5_metadata.py)

### Two useful upstream fixture notes

While bringing up the Python K5 dumper I found two upstream/tooling rough
edges that do not invalidate the parity result, but are worth recording:

1. `VHSDecode.buildmetadata()` expects a logger with a `.status(...)` method,
   not just a plain stdlib `Logger`.
2. The live threaded decode path can emit a shutdown-race traceback from the
   serration thread pool after the metadata cases are already written.

The dumper now stubs `logger.status` and explicitly closes the decoder so this
noise does not distract from the actual parity result.

### Native-rate K5 parity

Artifact:

- [k5_metadata_native](/media/hunter/DATA/captures/k5_metadata_native)

Result:

- `case_count_equal = true`
- `python_case_count = 8`
- `cpp_case_count = 8`
- `output_mismatch_count = 0`
- `duplicateField_mismatch_count = 0`
- `writeField_mismatch_count = 0`

This is exact parity across the whole dumped native-rate metadata sequence.

### True 40 MHz K5 parity

Artifact:

- [k5_metadata_40m](/media/hunter/DATA/captures/k5_metadata_40m)

Result:

- `case_count_equal = true`
- `python_case_count = 8`
- `cpp_case_count = 8`
- `output_mismatch_count = 0`
- `duplicateField_mismatch_count = 0`
- `writeField_mismatch_count = 0`

This is also exact parity across the dumped true-40 MHz metadata sequence.

### K5 checkpoint conclusion

The first K5 checkpoint is closed:

- native-rate metadata sequencing matches upstream exactly
- true 40 MHz metadata sequencing matches upstream exactly

So the next K5 work should move past `buildmetadata()` and into the remaining
write/json/output contract, not keep churning on field-order metadata logic.

## K5 completion: writeout / build_json contract

I finished the rest of K5 by porting the remaining VHS-specific output contract
around the metadata state machine:

- `VHSDecode.writeout()`
- `VHSDecode.build_json()`

Ported files:

- [cpp_port/include/vhsdecode_cpp/output_core.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/output_core.h)
- [cpp_port/src/output_core.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/output_core.cpp)
- [cpp_port/src/k5_write_json_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k5_write_json_parity_main.cpp)
- [cpp_port/tools/dump_k5_write_json_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k5_write_json_python.py)
- [cpp_port/tools/compare_k5_write_json.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k5_write_json.py)

### What this closes

This stage now covers the remaining downstream behavior that sits immediately
after K4 field output:

- strip unused `audioSamples` metadata before write
- append field metadata to the field-info stream
- update `fields_written`
- write luma bytes
- conditionally write chroma bytes when `write_chroma` is enabled
- construct the final VHS-specific JSON wrapper on top of
  `lddecode.core.build_json()`
  - `black16bIre` / `white16bIre` level-adjust scaling
  - `PAL-M` system remap for `MPAL` / `NLINHA`
  - `tapeFormat`

### Useful upstream fixture notes

Two Python-side quirks showed up while building the parity harness:

1. `field.downscale()` for NTSC VHS returns `((luma, chroma), audio, efm)`,
   not just `(luma, chroma)`.
2. `decoder.fieldinfo` is not a plain list; it is an upstream ring-buffer
   helper (`FieldInfo`). The parity harness therefore uses `fieldinfo.read()`
   to capture the emitted field rows and keeps `len(fieldinfo)` via the normal
   decoder state.

The same non-fatal live-decode shutdown-race traceback seen during K5 metadata
dumping also appeared here after the cases had already been written.

### Native-rate full K5 parity

Artifact:

- [k5_write_json_native](/media/hunter/DATA/captures/k5_write_json_native)

Result:

- `fieldinfo_equal = true`
- `fields_written_equal = true`
- `video_bytes_written_equal = true`
- `chroma_bytes_written_equal = true`
- `build_json_equal = true`

### True 40 MHz full K5 parity

Artifact:

- [k5_write_json_40m](/media/hunter/DATA/captures/k5_write_json_40m)

Result:

- `fieldinfo_equal = true`
- `fields_written_equal = true`
- `video_bytes_written_equal = true`
- `chroma_bytes_written_equal = true`
- `build_json_equal = true`

### K5 conclusion

K5 is complete for the exercised NTSC VHS path:

- metadata sequencing parity is exact on native-rate and true 40 MHz
- writeout parity is exact on native-rate and true 40 MHz
- final JSON-wrapper parity is exact on native-rate and true 40 MHz

So the next work should move downstream into K6, not revisit K5 unless we
later expand into additional tape families / color systems / audio branches.

## K6 completion: readfield() write orchestration

K6 closes the next real state machine downstream of K5: the write-orchestration
tail of `VHSDecode.readfield()`.

This is the layer that sits on top of the already-ported K1-K5 pieces and
decides, field by field:

- suppress leading second-field output until a first field exists
- cache the last valid field for each parity (`False` / `True`)
- handle duplicate-filler writes when the metadata layer requests them
- update `lastFieldWritten`
- feed the correct datasets into the K5 writeout contract

### Ported files

- [cpp_port/include/vhsdecode_cpp/session_core.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/session_core.h)
- [cpp_port/src/session_core.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/session_core.cpp)
- [cpp_port/src/k6_session_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k6_session_parity_main.cpp)
- [cpp_port/tools/dump_k6_session_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k6_session_python.py)
- [cpp_port/tools/compare_k6_session.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k6_session.py)

### Two useful harness fixes

Two test-harness bugs surfaced while validating K6:

1. The first compare used element counts for luma/chroma payload sizes, but
   upstream `writeout()` writes raw bytes. The harness now uses `nbytes`.
2. The first compare also mixed "newly emitted fieldinfo rows this step" with
   "cumulative session state". The harness now records cumulative fieldinfo so
   it matches the C++ controller state we actually care about.

These were fixture/unit issues, not K6 controller bugs.

### Native-rate K6 parity

Artifact:

- [k6_session_native](/media/hunter/DATA/captures/k6_session_native)

Result:

- `case_count_equal = true`
- `mismatch_count = 0`

So the native-rate `readfield()` write-orchestration state machine matches
upstream exactly on the dumped session trace.

### True 40 MHz K6 parity

Artifact:

- [k6_session_40m](/media/hunter/DATA/captures/k6_session_40m)

Result:

- `case_count_equal = true`
- `mismatch_count = 0`

So the same `readfield()` orchestration logic also matches upstream exactly on
the true-40 MHz EP fixture lane.

### One upstream fixture note

The live Python decode path still emits a late serration thread-pool shutdown
traceback after the session cases are already written. That noise did not affect
the recorded K6 parity artifacts and is treated as an upstream fixture/runtime
rough edge rather than a parity failure.

### K6 conclusion

K6 is complete for the exercised NTSC VHS path:

- native-rate `readfield()` write orchestration matches upstream exactly
- true 40 MHz `readfield()` write orchestration matches upstream exactly

At this point the port has exact or near-exact coverage from K1 through K6 on
the active NTSC VHS lane.

## K7 start: VHS readfield rerun / AGC control

K7 begins at the next still-open control layer inside `VHSDecode.readfield()`:
the rerun / acceptance logic that decides whether a decoded field is accepted
immediately or forces a redo.

### Important scope clarification

For the exercised VHS path, upstream `VHSDecode.checkMTF()` is hardwired:

- `return True`

So the meaningful K7 logic for VHS is **not** the broader `lddecode` MTF policy
branch. It is the VHS-specific control around:

- `f.needrerun`
- AGC-triggered redo
- `fdoffset` rewind target
- `adjusted` one-redo-only behavior
- `demodcache.flush_demod()` trigger
- acceptance vs deferred retry

### Ported files

- [cpp_port/include/vhsdecode_cpp/readfield_control_core.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/readfield_control_core.h)
- [cpp_port/src/readfield_control_core.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/readfield_control_core.cpp)
- [cpp_port/src/k7_control_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k7_control_parity_main.cpp)
- [cpp_port/tools/dump_k7_control_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k7_control_python.py)
- [cpp_port/tools/compare_k7_control.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k7_control.py)

### Exercised parity cases

The first K7 parity rung is a forced-case harness that covers the meaningful
VHS branches directly:

- EOF / `offset is None`
- invalid field with offset advance
- valid field with no redo
- explicit `needrerun`
- `needrerun` on already-adjusted pass
- AGC malfunction branch that must *not* update levels
- AGC branch that updates levels and requests redo
- AGC branch on the second pass where redo is no longer allowed

Artifact:

- [k7_control](/media/hunter/DATA/captures/k7_control)

Result:

- `case_count_equal = true`
- `mismatch_count = 0`

So the first K7 checkpoint is exact.

### K7 status

K7 is now underway with the first control kernel closed. The next logical K7
step is to fold this control kernel into a higher-level live-session trace so we
can validate the surrounding `readfield()` loop behavior under real decode
conditions, not just forced branch cases.

### K7 live-session closure

I then extended K7 from the synthetic branch harness into a live control-trace
parity harness driven by real fields from both fixtures.

Additional files:

- [cpp_port/tools/dump_k7_live_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k7_live_python.py)
- [cpp_port/src/k7_live_control_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k7_live_control_parity_main.cpp)
- [cpp_port/tools/compare_k7_live_control.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k7_live_control.py)

This live harness runs the rerun / AGC controller over real decoded fields in
serial mode so the control trace stays easy to reason about.

Artifacts:

- Native-rate: [k7_live_native](/media/hunter/DATA/captures/k7_live_native)
- True 40 MHz: [k7_live_40m](/media/hunter/DATA/captures/k7_live_40m)

Results:

- native-rate:
  - `case_count_equal = true`
  - `mismatch_count = 0`
- true 40 MHz:
  - `case_count_equal = true`
  - `mismatch_count = 0`

One useful observation:

- on both live fixtures, the controller stayed on the normal accept path
- no natural reruns were triggered on these specific traces
- that is why K7 uses *both*:
  - forced branch cases for coverage
  - live traces for real-session validation

### K7 conclusion

K7 is complete for the exercised NTSC VHS path:

- forced rerun / AGC control cases match exactly
- native-rate live control trace matches exactly
- true 40 MHz live control trace matches exactly

So the next work should move beyond the `readfield()` rerun controller into K8.

## K8 completion: outer session / CLI loop

K8 closes the remaining exercised outer-session behavior in `vhsdecode.main`
for the NTSC VHS path.

This is not another signal kernel. It is the top-level decode-session control
around the already-ported K1-K7 path:

- startup `roughseek` choice
- NTSC `blackIRE = 7.5` init when not `ntscj`
- per-iteration "done" logic
- JSON dumper write cadence
- disk-space pause check trigger
- final completion message / exit status

### Ported files

- [cpp_port/include/vhsdecode_cpp/session_loop_core.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/session_loop_core.h)
- [cpp_port/src/session_loop_core.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/session_loop_core.cpp)
- [cpp_port/src/k8_session_loop_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k8_session_loop_parity_main.cpp)
- [cpp_port/tools/dump_k8_session_loop_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k8_session_loop_python.py)
- [cpp_port/tools/compare_k8_session_loop.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/compare_k8_session_loop.py)

### What K8 deliberately does and does not include

This checkpoint covers the exercised control decisions in the outer loop.

It does **not** attempt to re-port the whole argparse/UI surface or the real
`JSONDumper` multiprocessing implementation. Instead it ports the decisions the
outer loop makes about when those components are used.

That is intentional: the goal here is parity of decode-session behavior, not a
verbatim clone of Python argparse boilerplate.

### K8 parity result

Artifact:

- [k8_session_loop](/media/hunter/DATA/captures/k8_session_loop)

Result:

- `startup_equal = true`
- `step_count_equal = true`
- `step_mismatch_count = 0`
- `finalize_equal = true`

So the exercised outer-session control loop matches upstream exactly for the
ported decision surface.

### K8 conclusion

K8 is complete for the exercised NTSC VHS path.

At this point the main NTSC VHS decode path has been ported and parity-tested
through:

- K1 demod
- K2 sync / pulse / serration
- K3 chroma
- K4 downscale / field output
- K5 metadata / write / json
- K6 readfield write orchestration
- K7 rerun / AGC control
- K8 outer session loop control

What remains after K8 is no longer the main exercised NTSC VHS path. The
remaining work is in deferred branches, additional formats, audio-related
branches, and future native-rate / policy expansion.

## Post-K8: hot-path "car" integration and direct-input K1 fix

After K1-K8 reached parity in staged harnesses, the next problem was not
correctness but throughput. The replay harnesses were good for validation, but
they were absolutely dragging down any performance picture.

### Why the FPS numbers looked contradictory

There are several different benchmark shapes now, and they are **not**
measuring the same thing:

1. Harness replay composite (`K1-K4`)
- This is the old staged replay workflow: load fixture, compute, write artifacts.
- It produced roughly `2-3 fps`.
- This is a validation workflow number, not a real decoder number.

2. Harness compute-only composite (`K1-K4`)
- Load once, then repeatedly run compute only.
- This produced about `17-18 fps`.
- This is much closer to kernel-only single-core capability.

3. In-memory hot-path "car"
- A real same-process integrated path, not stage-by-stage replay.
- The first clean integrated `40 MHz` `K2->K4` car ran around `22.07 fps`.
- The first coherent native and `40 MHz` `K3->K4` cars ran about `10-11 fps`.

4. Direct K1 raw-file stream bench
- This is a front-end block-rate benchmark, not a full field pipeline number.
- Native `~28.636 MHz`: `229.04` blocks/s, `4.37 ms` per block.
- Direct `40 MHz`: `129.22` blocks/s, `7.74 ms` per block.

So:

- `22 fps` was the fastest single-core integrated `40 MHz` `K2->K4` car.
- `10-11 fps` was the coherent `K3->K4` field-output car.
- `129/229 blocks/s` are K1 front-end block throughput numbers.

These are all real numbers, but they refer to **different pipeline spans**.

### New files for hot-path integration

- [cpp_port/src/ntsc_vhs_hotpath_bench.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_hotpath_bench.cpp)
- [cpp_port/src/k1_file_stream_bench.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k1_file_stream_bench.cpp)

The hot-path bench proved that native-rate can work coherently in an
in-memory car and that the staged replay harnesses were not representative of
hot-path speed.

### Native K1 fixture correction

An important correction happened during front-end work:

- the earlier K1 parity dumper was still implicitly tied to the Python
  resample-first workflow
- so "native" K1 was not truly native yet

This was fixed by teaching the K1 dumper and parity runner to preserve direct
`u8` input when `--no-resample` is active:

- [cpp_port/tools/dump_k1_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k1_python.py)
- [cpp_port/src/k1_parity_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/k1_parity_main.cpp)

This matters because native-rate workflow is a first-class architectural goal,
not a retrofit.

### Precise K1 direct-40MHz bug

Once true direct-input K1 was exercised on the `40 MHz` archive-derived clip,
the luma path diverged badly even though filters, envelope, and chroma stayed
tight.

The debugging ladder for that issue was:

1. Confirm the divergence was in K1, not later kernels.
2. Confirm filters still matched.
3. Confirm `hilbert` stayed tight.
4. Show the split happened in raw FM demod, not in analytic-signal generation.
5. Split K1 demod into:
   - pre-spike demod
   - diffed-helper demod
   - post-spike demod
6. Fix each actual bug in turn.

### Bug 1: `unwrap_hilbert_cpp()` was wrong

The first implementation used absolute-angle subtraction and positive wrapping.
That worked on easier fixtures but failed badly on direct `40 MHz` input.

The fix was to mirror the effective upstream behavior via:

- conjugate-product phase delta:
  - `h[n] * conj(h[n-1])`
- positive wrap into `[0, 2*pi)`
- scale to Hz

File:

- [cpp_port/src/demod_block.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/demod_block.cpp)

### Bug 2: diffed-helper first-sample case

For the diffed-Hilbert helper path, sample `1` was still wrong after the main
unwrap fix.

The issue:

- when the previous diffed sample is exactly zero, the helper path must treat
  the current sample's absolute phase as the first delta
- using the generic conjugate-product path against a zero previous sample gives
  `0`, which is not what Python/Rust does there

This special case was added to `unwrap_hilbert_cpp()`.

### Bug 3: `replace_spikes_cpp()` policy mismatch

The last remaining direct-40MHz mismatch was not in demod math but in spike
replacement policy.

Python behavior:

- compute the list of spike indices once from the original `demod`
- iterate that frozen list while mutating the waveform

Old C++ behavior:

- iterate linearly and re-check `demod[i]` live after earlier replacements

That meant overlapping earlier replacements could suppress later ones, which
does **not** match Python.

Fix:

- snapshot `to_fix` first
- then iterate that fixed list

Again in:

- [cpp_port/src/demod_block.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/demod_block.cpp)

### Final direct-40MHz K1 parity after fixes

On the bad direct `40 MHz` EP block:

- pre-spike demod:
  - RMS `10.84495`
  - `0.00017307%` of Python signal RMS
- diffed helper demod:
  - RMS `10.36005`
  - `0.00017656%`
- post-spike demod:
  - RMS `10.50626`
  - `0.00020998%`
- final luma:
  - RMS `0.21284`
  - `0.00000453%`
- final `demod_05`:
  - RMS `0.13658`
  - `0.00000291%`

So the direct `40 MHz` K1 lane is back in family with the native lane.

### Direct K1 stream throughput

Native direct-file K1:

- `229.04` blocks/s
- `4.37 ms` per block

Direct `40 MHz` K1:

- `129.22` blocks/s
- `7.74 ms` per block

These are front-end block-rate numbers, not full field-output car FPS.

### Loud reminder for later

LOUD NOTE:

- Do **not** compare `2 fps`, `18 fps`, `22 fps`, `10 fps`, and `129/229 blocks/s`
  as though they are all the same benchmark.
- They refer to different pipeline spans and were produced in different
  benchmark shapes.
- Any future performance writeup must state clearly:
  - whether the number is replay-harness, compute-only, integrated car, or
    direct K1 block-rate
  - whether it is native-rate or `40 MHz`
  - whether it is per-block or per-field

## 2026-04-03: First real end-to-end `u8 -> TBC` decode

The live decode/session loop is now working well enough to produce actual
`.tbc`, `_chroma.tbc`, and `.json` output from raw `u8` input.

### Root cause of the broken live loop

The remaining blocker was not K1-K8 signal parity. It was the live
`decodefield()`/`readfield()` scheduler shape.

Two important upstream mismatches were found and fixed in
[cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp):

- native `inlinelen` had to be derived from the actual input sample rate
  rather than inherited from a mismatched replay fixture
- the live window reader had to mirror upstream block alignment behavior:
  - `blocksize = 32768`
  - native read window on this tape family/path is `622592` bytes

An upstream probe on `TAPE_1` confirmed the relevant runtime values:

- `rf.freq_hz = 28636363.636363637`
- `rf.linelen = 1820`
- `blocklen = 32768`
- `blockcut = 1024`
- `blockcut_end = 1024`
- `blocksize = 32768`
- `readlen = 622592`
- `bytes_per_field = 477750`
- `output_lines = 263`

Another important fix:

- `ResyncRuntime` state must persist across fields in the live loop
- recreating it every field threw away serration/level history and damaged
  scheduler behavior

### First successful smoke result

After the blocksize/readlen and persistent-resync fixes, the driver went from
repeated `reason=no_first_hsync` failures to immediate real field acceptance:

- field 1 accepted on attempt 1
- subsequent fields advanced correctly
- TBC output began writing as expected after field pairing/state warmup

### First full end-to-end decode result

Command:

```bash
./build_port/cpp_port/vhsdecode_decode \
  /media/hunter/DATA/captures/TAPE_1_1min_33pct.u8 \
  /media/hunter/DATA/captures/tape1_cpp_decode_full
```

Output files produced:

- `/media/hunter/DATA/captures/tape1_cpp_decode_full.tbc`
- `/media/hunter/DATA/captures/tape1_cpp_decode_full_chroma.tbc`
- `/media/hunter/DATA/captures/tape1_cpp_decode_full.tbc.json`

Sizes:

- luma TBC: `1687755160` bytes
- chroma TBC: `1687755160` bytes
- JSON: `620178` bytes

Reported final run stats:

- `fields_seen = 3406`
- `attempts = 3794`
- `fields_written = 3526`
- `elapsed_s = 756.344790537`
- `throughput_fields_per_s = 4.6618950036`
- `throughput_frames_per_s = 2.3309475018`

### How to interpret this number

This is the first honest wall-clock `u8 -> TBC` number for the new ported
workflow on the native-rate `TAPE_1` sample.

It is **not**:

- a replay harness number
- a compute-only hot-path number
- a K1 block-rate number

It **is**:

- real raw input
- real live session loop
- real field scheduling
- real output writing
- full wall-clock elapsed time

So this `2.33 frames/s` figure is the correct current baseline for the first
true end-to-end decoder, even though it is much slower than the stripped
hot-path/kernel numbers.

### Remaining performance work implied by this result

The port is now far enough along that performance work can focus on the real
decoder instead of synthetic harnesses.

The next optimization targets should be:

- reduce live loop/session overhead
- reduce output/write overhead where appropriate
- integrate the already-faster in-memory hot-path work into the real driver
- then reintroduce the chunk-worker/orchestrator architecture for aggregate FPS

## 2026-04-03: Live orchestration/state bug in end-to-end decode

The first full `u8 -> TBC` driver run produced real output, but the image was
visibly wrong in `ld-analyse` and the emitted JSON showed a broken field cadence.

Observed bad output before the fix:

- `fieldPhaseID`: `1,2,3,4,1,3,1,1,2,2,3,4,...`
- `isFirstField` also stuck/repeated in places

Upstream on the same sample stays on the expected cadence:

- `fieldPhaseID`: `1,2,3,4,1,2,3,4,...`
- `isFirstField`: `True,False,True,False,...`

### Root cause

The live driver in
[cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)
was bypassing the already-ported K5/K6 ordering rules and mutating metadata
history in the wrong order.

Two concrete bugs:

1. It called `build_metadata()` **before** honoring the upstream
   "skip leading second field" rule.
2. It updated metadata history for seen fields instead of only for fields
   actually written.

This corrupted `seqNo`, which then corrupted fallback `fieldPhaseID`, which in
turn corrupted field/chroma cadence even though the underlying kernels were
fine in isolation.

### Additional live scheduler fix

There was also an upstream `decodefield()` offset mismatch:

- upstream uses `offset = f.nextfieldoffset - (readloc - rawdecode["startloc"])`
- the live driver had been treating `nextfieldoffset` as though it were already
  the final scheduler offset

That caused repeated same-parity detections in the live loop.

After fixing both issues, short live decode checks now return to the expected
alternating cadence for long stretches, for example:

- `isFirstField`: `True,False,True,False,...`
- `fieldPhaseID`: `1,2,3,4,1,2,3,4,...`

There can still be occasional explicit `decodeFaults=4` compensation events in
the emitted metadata, which are part of the upstream field-order recovery logic
rather than the original broken-state failure mode.

## 2026-04-03: Why the real decoder is 2.33 fps instead of 18-22 fps

The apparent performance collapse is not one single mystery slowdown. It is
mostly the difference between benchmark shape and actual work performed.

### The `18-22 fps` numbers were not full decode numbers

Those faster numbers came from partial hot-path cars such as:

- `K2 -> K4`
- or `K3 -> K4`

They did **not** include:

- live native `K1`
- live `decodefield()` scheduling
- overlapping field-window re-demodulation
- actual `.tbc` / `_chroma.tbc` / `.json` output generation

### The real decoder does include live K1

The first true `u8 -> TBC` decoder includes the native front-end. On this path:

- per-field live read window is about `622592` bytes
- block size is `32768`
- so each field decode touches about `19-20` K1 blocks

That alone is a big jump in work versus the hot-path cars that started
downstream of K1.

### The current live driver is still missing literal `DemodCache.read()` reuse

This is the biggest current performance bottleneck.

Right now the live driver re-demodulates large overlapping K1 windows for every
field. Neighboring fields share a lot of the same source bytes, but that
overlap is not yet being reused the way upstream `demodcache.read()` does.

So the current end-to-end decoder is paying for:

- real K1
- plus redundant K1 over overlapping field windows

That is a much better explanation for the `22 fps` to `2.33 fps` gap than
output bandwidth.

### Output bandwidth is not the main bottleneck

The final run wrote about:

- `1.687 GB` luma TBC
- `1.687 GB` chroma TBC

over about `756 s`, which is only a few MB/s of sustained output. That is not
the kind of bandwidth number that explains the full slowdown by itself.

### Short version

The main reasons the real decoder is much slower than the hot-path cars are:

1. the hot-path cars did not include live K1 at all
2. the live decoder currently redoes too much overlapping K1 work because
   upstream-style `DemodCache` reuse is not ported literally yet
3. the real decoder also pays the actual session/scheduler/output costs that
   the hot-path benches intentionally skipped

## 2026-04-03: Live native image fidelity follow-up

The next phase after getting the first real `u8 -> TBC` loop alive was to stop
looking at image symptoms and restore the same quantitative parity discipline we
used for the kernels.

The key lesson from this round is:

- the remaining image problem is **not** an obvious K1/K2/K3/K4 math failure
- it is still in the live integration/session path
- but we now have much better evidence for exactly what is and is not cleared

### What is now cleared quantitatively

#### Live K1 / demodcache output is tight

Using the live `DemodCache.read()` substrate at `readloc=0`:

- RMS diff: `0.23601172603454176`
- relative error: `0.000006075609087232705%` of Python signal RMS

This means the live K1 path is not the current image blocker.

#### `get_pulses()` live substrate is tight

On the exact live demod / demod_05 substrate:

- Python pulse count: `407`
- C++ pulse count: `407`
- first starts matched exactly (`225`, `2044`, `3864`, ...)

Post-`get_pulses()` demod arrays were also still tight:

- `demod` RMS diff: `57.657818246181286`
- relative error: `0.0015067080312058763%`

So raw pulse finding / resync level correction is not the remaining issue.

#### `refine_pulses()` is exact on the live substrate

On the saved live refine fixture:

- Python valid pulse count: `405`
- C++ valid pulse count: `405`
- mismatch count: `0`

So pulse classification is also not the remaining issue.

### Important fixture bug found and fixed

The earlier large live `refine_linelocs_hsync()` mismatch turned out to be a
fixture bug, not a bad kernel.

The Python dumpers in:

- [cpp_port/tools/dump_k2_live_fixture.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k2_live_fixture.py)
- [cpp_port/tools/dump_k2_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k2_python.py)
- [cpp_port/tools/dump_k2_metadata_hit.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k2_metadata_hit.py)

were passing a temporary `np.asarray(linebad, dtype=np.uint8)` into
`sync.refine_linelocs_hsync(...)` but then writing the original Python-side
list copy back out as `python_linebad.u8`.

That meant the saved fixture was internally inconsistent:

- refined Python `linelocs2`
- but stale pre-refine `linebad`

After fixing the dumper to mutate and persist the same `uint8` array object,
the live HSYNC-refine comparison collapsed to a tight result:

- `linelocs2` RMS diff: `0.06449067716595075`
- `linebad` mismatch count: `0`

So `refine_linelocs_hsync()` is now effectively cleared too.

### Live driver fixes that were already in place

Before the field-level image compare, the live driver in
[cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)
had already received several faithful-port fixes:

1. `decode_field()` read window uses upstream-style input-domain `rf.linelen`
   instead of K4/output `inlinelen`
2. K2/refine uses input-domain `rf_linelen_i`, not K4/output `inlinelen`
3. HSYNC refine uses `resync.last_pulse_threshold()` like upstream active NTSC
   VHS path
4. the driver carries the resync-corrected live demod forward into K4
5. K4 luma downscale uses normal `linelocs`, not burst/phase-shifted
   `linelocs_burst`
6. live field cadence / `fieldPhaseID` ordering bug was fixed earlier

These changes materially improved the driver and removed the worst broken-field
state failure mode.

### What is still wrong end-to-end

Even with the above fixes, the integrated live driver output is still not close
enough to upstream.

On a direct native `u8 -> TBC` compare against upstream's 40-field native run:

- luma RMS diff: `3208.165803297214`
- luma relative error: `11.805527577013489%` of upstream signal RMS
- chroma RMS diff: `2820.262323710186`
- chroma relative error: `8.590497986284399%`

Important metadata state of that same compare:

- `isFirstField` mismatches: `0`
- `fieldPhaseID` mismatches: `0`
- `decodeFaults` mismatches: `0`
- `fileLoc` mismatches: `7`
- `syncConf` mismatches: `39`
- field counts: upstream `40`, C++ `39`

So the earlier catastrophic cadence corruption is gone, but the stream is still
not faithful enough to upstream for image fidelity.

### Why the first direct field oracle was misleading

The standalone `decodefield(0)` oracle in:

- [k4_field_native_decodefield0](/media/hunter/DATA/captures/k4_field_native_decodefield0)

is useful for local debugging, but it is **not** the right first output-field
oracle for the live `readfield()` driver stream. The first valid live driver
field at `readloc=0` is a different substrate than the first emitted field in
the upstream native output stream.

This matters because the first actual written upstream field in the native
40-field run starts at:

- `fileLoc = 583680`

not at `readloc = 0`.

So the correct next comparison is:

- first emitted C++ field
vs
- first emitted upstream live field from the same native decode stream

and not:

- C++ live field
vs
- standalone `decodefield(0)` field

### Current working hypothesis

At this point the remaining bug class is much narrower:

- the isolated kernels look good
- the resync / pulse / HSYNC refine kernels are no longer the obvious culprit
- the remaining error is almost certainly in the live integration/session path
  that decides which exact field object gets emitted and with what final live
  state

In other words:

- the picture is still wrong
- but the remaining problem is now much more likely to be a live
  `readfield()/session` fidelity issue than a hidden K1-K4 arithmetic bug

### Next exact step

The next compare to run is a one-to-one live field-output check:

1. take the first emitted C++ field from the real native `u8 -> TBC` stream
2. produce the corresponding first emitted upstream live field from the same
   native stream
3. compare `u16` luma/chroma directly for that exact field

That will tell us whether:

- the per-field image itself is still wrong
or
- the remaining full-stream mismatch is mostly field-selection / stream-alignment

This is the current active debug target.

## 2026-04-03 - Live Native Image Fidelity Closed Quantitatively

The native live image mismatch is no longer a vague “looks wrong” problem. I
walked it down to a sequence of faithful-port bugs in the integrated live
driver and got the emitted image numerically close to upstream `vhs-decode`.

### First hard fix: native Python live oracle had to be truly native

Several earlier “native” Python dump paths were accidentally still calling:

- `lddu.make_loader(path, inputfreq)`

which silently resamples to `40 MHz` when `inputfreq != 40`. For true
`--no-resample` native work, the loader must be created with:

- `lddu.make_loader(path)`

and the native rate only passed to `VHSDecode(..., inputfreq=native_rate)`.

This was corrected in:

- [cpp_port/tools/dump_live_written_field_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_live_written_field_python.py)
- [cpp_port/tools/dump_k4_field_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_k4_field_python.py)

### Second hard fix: K2 startup scheduler was off by exactly one line

The startup throwaway field at `readloc=0` matched upstream on:

- `first_hsync_loc`
- `first_hsync_loc_line`
- `meanlinelen`

but C++ projected `next_field` one full line too far forward. The exact bug was:

- `first.field_lines = {262, 263}`

when upstream NTSC expects:

- `field_lines = [263, 262]`

in:

- [cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)

After fixing that, the startup jump now matches upstream exactly:

- upstream startup `nextfieldoffset = 615301`
- C++ startup `next_offset = 615301`

### Third hard fix: live NTSC field geometry was missing the real post-K2 steps

The live driver was skipping part of the active upstream NTSC VHS geometry
contract. Upstream `FieldNTSCShared` / `FieldNTSC` still does:

1. burst-locked lineloc adjustment
2. bad-line repair
3. `-83°` NTSC phase offset

before final picture output.

That sequence is now applied in the C++ live driver before final K4 output in:

- [cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)

After that fix, the first written field’s final live linelocs matched the true
native upstream live field very tightly:

- `lineloc RMS = 0.08395194985560733`
- `lineloc mean = -0.025120575862777616`
- `lineloc max = 0.9686657159836614`

### Fourth hard fix: live luma must use the post-resync demod substrate

I had temporarily forced the live picture path to use the pristine pre-resync
K1 demod buffer. That was wrong for the active live VHS path.

The proof was direct:

- upstream first written field mean demod: `3826780.689115661`
- C++ original window mean: `3880008.041165386`
- C++ post-resync window mean: `3822103.041478723`

That `~58 kHz` difference maps almost exactly to the washed-out brightness
offset we were seeing.

The live driver now feeds final luma output from the same post-resync substrate
upstream uses in:

- [cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)

This collapsed first-field luma error from:

- RMS `3152.7430261006975`

down to:

- RMS `200.721599116327`

with:

- mean bias `190.2708018217524`
- correlation `0.99999690585382`

### Fifth hard fix: final chroma TBC must use final field linelocs

The live driver was reusing the pre-final chroma TBC buffer for final output.
That was not faithful to upstream.

Upstream does:

- phase detection during burst refine using pre-final linelocs
- then final `decode_chroma()` re-runs `Field.downscale(channel="demod_burst")`
  on the field’s final post-offset linelocs

The live driver now mirrors that two-stage contract in:

- [cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)

This collapsed first-field chroma error from:

- RMS `859.8468449109392`

down to:

- RMS `10.8834528827513`

with:

- mean `0.001529269209877575`
- correlation `0.9999858113195171`

### Current quantitative live image match

Using:

- C++ live output:
  - [tape1_cpp_probe20.tbc](/media/hunter/DATA/captures/tape1_cpp_probe20.tbc)
  - [tape1_cpp_probe20_chroma.tbc](/media/hunter/DATA/captures/tape1_cpp_probe20_chroma.tbc)
  - [tape1_cpp_probe20.tbc.json](/media/hunter/DATA/captures/tape1_cpp_probe20.tbc.json)
- upstream live output:
  - [py2.tbc](/media/hunter/DATA/captures/live_written_field_python_true7/py2.tbc)
  - [py2_chroma.tbc](/media/hunter/DATA/captures/live_written_field_python_true7/py2_chroma.tbc)

the first two written native fields now compare as:

- luma RMS `220.675619807188`
- luma mean `183.70545272218277`
- luma correlation `0.9999380716338202`
- luma RMS error = `0.3367294114704936%` of full `u16` scale
- luma RMS error = `0.9056522847385571%` of upstream luma signal RMS

- chroma RMS `194.4973613928185`
- chroma mean `-0.000215184055488238`
- chroma correlation `0.9955511119624827`
- chroma RMS error = `0.29678394963426946%` of full `u16` scale
- chroma RMS error = `0.5924167930207294%` of upstream chroma signal RMS

For the first written field alone, the match is even tighter:

- luma RMS `200.721599116327`
- luma correlation `0.99999690585382`
- chroma RMS `10.8834528827513`
- chroma correlation `0.9999858113195171`

### Remaining caveat

There is still a live metadata/readloc mismatch on the first written C++ field:

- C++ first written `fileLoc = 614277`
- upstream first written `fileLoc = 583680`

But despite that metadata discrepancy, the actual emitted first-field image
content and geometry are now quantitatively close to upstream.

So the current state is:

- live native image fidelity: quantitatively demonstrated
- remaining gap: live stream/readloc metadata fidelity is still not perfect

## 2026-04-03 - Live `fileLoc` Contract Fixed Too

The remaining live session-fidelity gap was not in picture generation anymore.
It was a coordinate-system mismatch in the C++ driver between:

- the scheduler offset (`fdoffset` / `start`)
- the demod-cache read origin (`rawdecode["startloc"]`)

Upstream `decodefield()` does:

- `readloc = int(start - self.rf.blockcut)`
- `rawdecode = demodcache.read(...)`
- `Field(..., readloc=rawdecode["startloc"])`

So the field object's emitted `readloc/fileLoc` is **not** the scheduler's
requested `start`; it is the cache window origin.

### What was wrong in C++

I had temporarily “fixed” the driver by treating the scheduler offset itself as
the field `readloc`, which made the first written field appear at:

- `615301`

instead of upstream's:

- `583680`

The correct faithful-port behavior is:

1. keep the upstream-style `start - blockcut` decode entry math
2. but emit `field.readloc = window->startloc`

That fix is now in:

- [cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)

### Current first two written field positions

From the corrected live run:

- [tape1_cpp_probe24.tbc.json](/media/hunter/DATA/captures/tape1_cpp_probe24.tbc.json)

C++ now emits:

- first written field:
  - `fileLoc = 583680`
  - `fieldPhaseID = 1`
  - `isFirstField = true`
  - `syncConf = 45`
- second written field:
  - `fileLoc = 1075200`
  - `fieldPhaseID = 2`
  - `isFirstField = false`
  - `syncConf = 45`

Those match the upstream live trace values we captured earlier.

### Current live quantitative fidelity after the `fileLoc` fix

Using:

- C++:
  - [tape1_cpp_probe24.tbc](/media/hunter/DATA/captures/tape1_cpp_probe24.tbc)
  - [tape1_cpp_probe24_chroma.tbc](/media/hunter/DATA/captures/tape1_cpp_probe24_chroma.tbc)
- upstream:
  - [py2.tbc](/media/hunter/DATA/captures/live_written_field_python_true7/py2.tbc)
  - [py2_chroma.tbc](/media/hunter/DATA/captures/live_written_field_python_true7/py2_chroma.tbc)

The first two written native fields compare as:

- luma RMS `220.675619807188`
- luma mean `183.70545272218277`
- luma corr `0.9999380716338202`
- luma RMS error = `0.3367294114704936%` of full `u16` scale
- luma RMS error = `0.9056522847385571%` of upstream luma signal RMS

- chroma RMS `194.4973613928185`
- chroma mean `-0.000215184055488238`
- chroma corr `0.9955511119624827`
- chroma RMS error = `0.29678394963426946%` of full `u16` scale
- chroma RMS error = `0.5924167930207294%` of upstream chroma signal RMS

So at this point both are true:

- live image fidelity is quantitatively demonstrated
- live `fileLoc`/field-position fidelity is also corrected for the first written
  fields

## 2026-04-03: exact 40-field live native compare, phase-4 burst refine bug

I finished the first broad like-for-like native live compare instead of relying
on the earlier mismatched minute-long upstream output.

### Exact upstream baseline

I updated:

- [cpp_port/tools/dump_live_written_field_python.py](/media/hunter/DATA/GitHub/VHSpp/cpp_port/tools/dump_live_written_field_python.py)

so it now:

- captures emitted `fi` rows directly in `writeout()`
- writes a real `python_live_capture.tbc.json`
- supports `--capture-seq N` for dumping a specific written field
- stubs `logger.status = logger.info` so live upstream capture no longer spews
  repeated VBI logger attribute errors

That produced the exact 40-field upstream live baseline at:

- [/media/hunter/DATA/captures/live_python_cmp40_exact2/python_live_capture.tbc](/media/hunter/DATA/captures/live_python_cmp40_exact2/python_live_capture.tbc)
- [/media/hunter/DATA/captures/live_python_cmp40_exact2/python_live_capture_chroma.tbc](/media/hunter/DATA/captures/live_python_cmp40_exact2/python_live_capture_chroma.tbc)
- [/media/hunter/DATA/captures/live_python_cmp40_exact2/python_live_capture.tbc.json](/media/hunter/DATA/captures/live_python_cmp40_exact2/python_live_capture.tbc.json)

The first three emitted upstream fields are:

- `583680 / phase 1 / first`
- `1075200 / phase 2 / second`
- `1536000 / phase 3 / first`

matching the corrected C++ live session path.

### Broad compare before the burst-refine fix

Comparing:

- [tape1_cpp_decode_cmp40d.tbc](/media/hunter/DATA/captures/tape1_cpp_decode_cmp40d.tbc)
- [tape1_cpp_decode_cmp40d_chroma.tbc](/media/hunter/DATA/captures/tape1_cpp_decode_cmp40d_chroma.tbc)
- [tape1_cpp_decode_cmp40d.tbc.json](/media/hunter/DATA/captures/tape1_cpp_decode_cmp40d.tbc.json)

against the exact upstream 40-field live run gave:

- metadata mismatches across the compared 39 fields:
  - `fileLoc = 0`
  - `fieldPhaseID = 0`
  - `isFirstField = 0`
  - `syncConf = 0`
- luma:
  - RMS `276.1030129471121`
  - mean `178.09965812680056`
  - corr `0.9997817353042031`
  - `1.132986036124751%` of upstream signal RMS
- chroma:
  - RMS `322.9198029995405`
  - mean `0.0016938311761359435`
  - corr `0.9874930317599456`
  - `0.9836113054911866%` of upstream signal RMS

So the broad live run was already much better than the old mismatched
minute-long compare, but luma was still slightly above 1% and chroma was only
just under.

### What the broad-run outliers showed

Field-level ranking showed the worst residual errors clustered in a handful of
fields, especially phase-4 fields. That made the remaining issue look like a
specific live geometry/phase refinement bug rather than a general decode drift.

Worst fields before the fix included:

- luma:
  - field `5`: `2.699066764904063%`
  - field `8`: `2.602897389439964%`
  - field `16`: `1.9778808842784514%`
  - field `32`: `1.7186083915038146%`
- chroma:
  - field `16`: `3.6026766743402763%`
  - field `8`: `2.7168603998566523%`

### Targeted field dump on the bad phase-4 case

I added targeted field dump selection for the C++ live decoder:

- [cpp_port/include/vhsdecode_cpp/decode_driver.h](/media/hunter/DATA/GitHub/VHSpp/cpp_port/include/vhsdecode_cpp/decode_driver.h)
- [cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)

via:

- `--debug-capture-seq N`

Using the same written field (`seq 8`) from both decoders:

- C++ debug:
  - [/media/hunter/DATA/captures/cpp_debug_field8/cpp_linelocs.f64](/media/hunter/DATA/captures/cpp_debug_field8/cpp_linelocs.f64)
  - [/media/hunter/DATA/captures/cpp_debug_field8/cpp_linebad.u8](/media/hunter/DATA/captures/cpp_debug_field8/cpp_linebad.u8)
  - [/media/hunter/DATA/captures/cpp_debug_field8/cpp_luma_u16.u16](/media/hunter/DATA/captures/cpp_debug_field8/cpp_luma_u16.u16)
  - [/media/hunter/DATA/captures/cpp_debug_field8/cpp_chroma_u16.u16](/media/hunter/DATA/captures/cpp_debug_field8/cpp_chroma_u16.u16)
- Python debug:
  - [/media/hunter/DATA/captures/py_debug_field8/python_linelocs.f64](/media/hunter/DATA/captures/py_debug_field8/python_linelocs.f64)
  - [/media/hunter/DATA/captures/py_debug_field8/python_linebad.u8](/media/hunter/DATA/captures/py_debug_field8/python_linebad.u8)
  - [/media/hunter/DATA/captures/py_debug_field8/python_luma_u16.u16](/media/hunter/DATA/captures/py_debug_field8/python_luma_u16.u16)
  - [/media/hunter/DATA/captures/py_debug_field8/python_chroma_u16.u16](/media/hunter/DATA/captures/py_debug_field8/python_chroma_u16.u16)

Before the fix this specific field showed:

- `linelocs` RMS `2.6632543932924118`
- `linebad` mismatches `2`
  - C++ bad lines: `3 4 5 6 261 262`
  - Python bad lines: `3 4 5 6 260 261`
- luma:
  - RMS `627.8242782230105`
  - mean `179.5899427568629`
  - corr `0.9980341471446352`
  - `2.602897389439964%` of signal RMS
- chroma:
  - RMS `892.2087340949633`
  - corr `0.921775219861485`
  - `2.7168603998566523%` of signal RMS

The `linelocs` deltas were the smoking gun:

- large `~ -8 sample` shifts on many lines:
  - `10..25`
  - `28..34`
  - `36`
  - `46`
  - `54`
  - `58`
  - `259`
  - `261..262`

### Root cause: simplified `sync_to_burst()` was not faithful enough

My previous live NTSC burst refinement in:

- [cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)

was still a shortcut:

- directly apply per-line phase adjustment where burst data exists
- do nothing upstream-like for missing/outlier lines

Upstream `FieldNTSC.refine_linelocs_burst()` does more:

1. compute per-line burst adjustments
2. compute the median adjustment
3. reject outlier adjustments outside `±2`
4. carry the last valid adjustment forward across lines
5. mark missing burst-adjustment lines as bad

I replaced the live C++ `sync_to_burst()` with a closer port of that logic:

- build per-line adjustment map
- compute median adjustment
- apply only inlier adjustments
- carry `lastvalid_adj` across missing/outlier lines
- mark missing adjustment lines bad before `fix_badlines()`

I also added a small local `median_of()` helper and the field-selected C++
debug dump support noted above.

### Targeted bad-field result after the burst-refine fix

New C++ debug field 8:

- [/media/hunter/DATA/captures/cpp_debug_field8b/cpp_linelocs.f64](/media/hunter/DATA/captures/cpp_debug_field8b/cpp_linelocs.f64)
- [/media/hunter/DATA/captures/cpp_debug_field8b/cpp_linebad.u8](/media/hunter/DATA/captures/cpp_debug_field8b/cpp_linebad.u8)
- [/media/hunter/DATA/captures/cpp_debug_field8b/cpp_luma_u16.u16](/media/hunter/DATA/captures/cpp_debug_field8b/cpp_luma_u16.u16)
- [/media/hunter/DATA/captures/cpp_debug_field8b/cpp_chroma_u16.u16](/media/hunter/DATA/captures/cpp_debug_field8b/cpp_chroma_u16.u16)

Compared to the same upstream field:

- `linelocs` RMS improved from `2.6632543932924118` to `0.30001966442336825`
- `linelocs` max improved from `8.283031041442882` to `2.183979584136978`
- `linebad` mismatches stayed at `2`, still only the bottom-tail one-line offset
- luma improved:
  - RMS `198.72305742834843`
  - corr `0.9999751636474782`
  - `0.8238861499045771%` of signal RMS
- chroma improved:
  - RMS `80.38106099562425`
  - corr `0.9993350515429259`
  - `0.24476797096029068%` of signal RMS

### Broad 39-field exact live compare after the burst-refine fix

New C++ broad run:

- [tape1_cpp_decode_cmp40e.tbc](/media/hunter/DATA/captures/tape1_cpp_decode_cmp40e.tbc)
- [tape1_cpp_decode_cmp40e_chroma.tbc](/media/hunter/DATA/captures/tape1_cpp_decode_cmp40e_chroma.tbc)
- [tape1_cpp_decode_cmp40e.tbc.json](/media/hunter/DATA/captures/tape1_cpp_decode_cmp40e.tbc.json)

Performance:

- `fields_seen = 40`
- `fields_written = 39`
- `elapsed = 8.598233535 s`
- `4.535815390596971 fields/s`
- `2.2679076952984856 frames/s`

Broad exact live compare against the 40-field upstream baseline now gives:

- metadata mismatches across compared 39 fields:
  - `fileLoc = 0`
  - `fieldPhaseID = 0`
  - `isFirstField = 0`
  - `syncConf = 0`
- luma:
  - RMS `249.22294831582258`
  - mean `178.80916340167585`
  - corr `0.9998579636578783`
  - `1.0226839515791706%` of upstream signal RMS
  - `0.3802898425510377%` of full `u16` scale
- chroma:
  - RMS `223.44302839690383`
  - mean `0.0002219872357339453`
  - corr `0.9940173884119711`
  - `0.6806057938314041%` of upstream signal RMS
  - `0.3409522062972516%` of full `u16` scale

So the broad live run is now in a much healthier state:

- stream/session metadata is exact over the compared range
- chroma is comfortably below `1%`
- luma is now only very slightly above `1%`

### Remaining shape of the error

The residual broad-run luma mismatch is no longer dominated by the old phase-4
bug. It now looks like:

- a stable positive luma bias of about `+178` codes
- plus a smaller set of heavier fields

Worst remaining luma fields after the burst-refine fix:

- `17`: `1.602450835713529%`
- `23`: `1.5651025714664906%`
- `15`: `1.5460404522964282%`
- `37`: `1.5147222293373597%`
- `16`: `1.4530019187645347%`
- `20`: `1.4480115187202358%`

Worst remaining chroma fields after the fix:

- `17`: `1.9970401675163891%`
- `18`: `1.1823576326707506%`
- `16`: `1.1788552992006134%`
- `6`: `1.1647824570283152%`
- `32`: `1.13221135834516%`

So the broad native live port is now quantitatively close and structurally
faithful. The remaining cleanup work is to shave the last small luma bias and
the few remaining heavier fields, not to rediscover the big integration bugs.

### Broad 39-field exact live compare after dynamic live luma-level recovery

The remaining broad luma error turned out to be mostly a live output-conversion
state issue, not a geometry issue. Upstream updates `ire0`, `hz_ire`, and
`vsync_ire` during `readfield()` before the first written fields, and the C++
driver was still effectively converting luma with static/default output levels.

I added a live NTSC level recovery step in the end-to-end driver that derives
the output conversion state from the pre-burst geometry substrate instead of
reusing stale/static values. This keeps the live driver faithful to upstream's
dynamic field state instead of papering over the output with a fixed offset.

New C++ broad run:

- [tape1_cpp_decode_cmp40g.tbc](/media/hunter/DATA/captures/tape1_cpp_decode_cmp40g.tbc)
- [tape1_cpp_decode_cmp40g_chroma.tbc](/media/hunter/DATA/captures/tape1_cpp_decode_cmp40g_chroma.tbc)
- [tape1_cpp_decode_cmp40g.tbc.json](/media/hunter/DATA/captures/tape1_cpp_decode_cmp40g.tbc.json)

Broad exact live compare against the same 40-field upstream baseline:

- metadata mismatches across compared 39 fields:
  - `fileLoc = 0`
  - `fieldPhaseID = 0`
  - `isFirstField = 0`
  - `syncConf = 0`
- luma:
  - RMS `167.86472859524622`
  - mean `11.333812341504649`
  - corr `0.9998517005609633`
  - `0.6888312859255759%` of upstream signal RMS
  - `0.2561451569317864%` of full `u16` scale
- chroma:
  - RMS `223.44302839690383`
  - mean `0.0002219872357339453`
  - corr `0.9940173884119711`
  - `0.6806057938314041%` of upstream signal RMS
  - `0.3409522062972516%` of full `u16` scale

This is the first broad native live compare where both luma and chroma are
below `1%` of upstream signal RMS while keeping stream/session metadata exact
across the whole compared run.

The remaining visible work is no longer "is the integrated port faithful?" The
answer is now yes, at least over this broad native live window. What remains is:

- extend the validation window further than the first 39 matched fields
- identify whether the remaining chroma floor is just normal field-to-field
  residual or one more small live-state mismatch
- move back to throughput work with a trustworthy live image baseline

### 99-field exact live compare after the dynamic-level fix

I pushed the like-for-like native live comparison out to the first ~100 written
fields using an exact upstream live baseline and the current C++ live decoder:

- upstream:
  - [python_live_capture.tbc](/media/hunter/DATA/captures/live_python_cmp100_exact/python_live_capture.tbc)
  - [python_live_capture_chroma.tbc](/media/hunter/DATA/captures/live_python_cmp100_exact/python_live_capture_chroma.tbc)
  - [python_live_capture.tbc.json](/media/hunter/DATA/captures/live_python_cmp100_exact/python_live_capture.tbc.json)
- C++:
  - [tape1_cpp_decode_cmp100a.tbc](/media/hunter/DATA/captures/tape1_cpp_decode_cmp100a.tbc)
  - [tape1_cpp_decode_cmp100a_chroma.tbc](/media/hunter/DATA/captures/tape1_cpp_decode_cmp100a_chroma.tbc)
  - [tape1_cpp_decode_cmp100a.tbc.json](/media/hunter/DATA/captures/tape1_cpp_decode_cmp100a.tbc.json)

Compared range:

- upstream fields written: `100`
- C++ fields written: `99`
- exact field-for-field compare range: `99`

Metadata stayed exact across the full compared range:

- `fileLoc = 0` mismatches
- `fieldPhaseID = 0`
- `isFirstField = 0`
- `syncConf = 0`

Image fidelity also held below `1%` of upstream signal RMS on both channels:

- luma:
  - RMS `186.6645960486886`
  - mean `-13.670908938969776`
  - corr `0.9998270244873559`
  - `0.7522852242478841%` of upstream signal RMS
  - `0.28483191584449313%` of full `u16` scale
- chroma:
  - RMS `210.70540219775626`
  - mean `0.0008754658944773013`
  - corr `0.9946484388573946`
  - `0.6418141645463906%` of upstream signal RMS
  - `0.3215158345887789%` of full `u16` scale

This is a much stronger claim than the earlier short-window probes. The live
native decoder is now quantitatively faithful to upstream over a materially
broader run, not just the first few written fields.

### Throughput baseline with the now-trustworthy live image path

The same 100-field C++ run took:

- `elapsed_s = 20.965629988`
- `throughput_fields_per_s = 4.7220140800283215`
- `throughput_frames_per_s = 2.3610070400141607`

Stage timing from the live decoder:

- `read_s = 5.602676`
- `k2_s = 9.125172`
- `k3_s = 0.941576`
- `k4_s = 4.054197`
- `write_s = 0.026078`

So with image fidelity now validated over the broader run, the next honest
performance work should target:

1. `K2` live sync/field scheduling
2. live `read`/K1 path
3. `K4`

Write bandwidth remains negligible.

### Full-minute / full-clip inspection outputs: luma bias and chroma sanity

After the earlier native `cmp100a` validation, I generated full inspection TBCs
for side-by-side visual review:

- native:
  - [TAPE_1_1min_33pct_cpp_inspect.tbc](/media/hunter/DATA/captures/TAPE_1_1min_33pct_cpp_inspect.tbc)
  - [TAPE_1_1min_33pct_cpp_inspect_chroma.tbc](/media/hunter/DATA/captures/TAPE_1_1min_33pct_cpp_inspect_chroma.tbc)
  - [TAPE_1_1min_33pct_cpp_inspect.tbc.json](/media/hunter/DATA/captures/TAPE_1_1min_33pct_cpp_inspect.tbc.json)
  - [TAPE_1_1min_33pct_vhsdecode_inspect.tbc](/media/hunter/DATA/captures/TAPE_1_1min_33pct_vhsdecode_inspect.tbc)
  - [TAPE_1_1min_33pct_vhsdecode_inspect_chroma.tbc](/media/hunter/DATA/captures/TAPE_1_1min_33pct_vhsdecode_inspect_chroma.tbc)
  - [TAPE_1_1min_33pct_vhsdecode_inspect.tbc.json](/media/hunter/DATA/captures/TAPE_1_1min_33pct_vhsdecode_inspect.tbc.json)
- 40 MHz:
  - [wcbs_40m_cpp_inspect.tbc](/media/hunter/DATA/captures/wcbs_40m_cpp_inspect.tbc)
  - [wcbs_40m_cpp_inspect_chroma.tbc](/media/hunter/DATA/captures/wcbs_40m_cpp_inspect_chroma.tbc)
  - [wcbs_40m_cpp_inspect.tbc.json](/media/hunter/DATA/captures/wcbs_40m_cpp_inspect.tbc.json)
  - [wcbs_40m_vhsdecode_inspect.tbc](/media/hunter/DATA/captures/wcbs_40m_vhsdecode_inspect.tbc)
  - [wcbs_40m_vhsdecode_inspect_chroma.tbc](/media/hunter/DATA/captures/wcbs_40m_vhsdecode_inspect_chroma.tbc)
  - [wcbs_40m_vhsdecode_inspect.tbc.json](/media/hunter/DATA/captures/wcbs_40m_vhsdecode_inspect.tbc.json)

These inspection outputs do **not** match the earlier comforting `cmp100a`
story on luma. Direct field-matched sampling shows a systematic darker luma in
VHSpp on both samples, while chroma remains centered and looks healthy.

#### Random matched-field luma sampling: native `TAPE_1`

Sampling method:

- exact matched fields only:
  - `fileLoc`
  - `fieldPhaseID`
  - `isFirstField`
- deterministic spread + random sample across the run
- compare per-field mean signed `cpp - python` on luma `u16`

Results:

- sampled fields: `45`
- matched fields total: `2880`
- negative mean count: `45`
- positive mean count: `0`
- zero mean count: `0`
- mean of sampled field means: `-3020.344813282488`
- median of sampled field means: `-3011.781866300397`

Percentage perspective:

- mean sampled luma bias: `-4.6091%` of full `u16` scale
- median sampled luma bias: `-4.5954%`

Representative native luma field means (`cpp - python`):

- field `1`: `-2976.9436592351885`
- field `35`: `-3068.32076849634`
- field `70`: `-2908.789042059597`
- field `105`: `-3096.9745230398153`
- field `500`: `-2839.9815011473843`
- field `1000`: `-2905.4947923820373`
- field `1500`: `-3179.2199996649974`
- field `1796`: `-3246.150131488585`

So the native full-minute inspection output is consistently darker by about
`~3k` codes.

#### Random matched-field luma sampling: 40 MHz WCBS clip

Same method as above, on the 40 MHz finished inspection outputs.

Results:

- sampled fields: `45`
- negative mean count: `45`
- positive mean count: `0`
- zero mean count: `0`
- mean of sampled field means: `-2115.7481357654533`
- median of sampled field means: `-2119.3132409842383`

Percentage perspective:

- mean sampled luma bias: `-3.2288%` of full `u16` scale
- median sampled luma bias: `-3.2343%`

Representative 40 MHz luma field means (`cpp - python`):

- field `1`: `-1946.3739836853654`
- field `20`: `-1845.2745381149396`
- field `64`: `-2302.7923083365436`
- field `70`: `-2547.833446675935`
- field `71`: `-2421.297003400278`
- field `72`: `-2412.942074672116`
- field `73`: `-2365.269034019531`
- field `105`: `-2310.461094454029`
- field `154`: `-2330.1480075710624`
- field `3029`: `-2188.9808076916634`

So the 40 MHz finished inspection output is also consistently darker, though by
less than the native minute sample (`~2.1k` codes rather than `~3.0k`).

#### Random matched-field chroma sampling: native `TAPE_1`

Same exact field-matching rule, but on `_chroma.tbc`.

Results:

- sampled fields: `45`
- matched fields total: `2880`
- negative mean count: `19`
- positive mean count: `26`
- zero mean count: `0`
- mean of sample means: `+0.0036058208591331986`
- median of sample means: `+0.00036515301251235325`
- mean sample MAE: `19.209827641078036`
- median sample MAE: `15.006184151019246`
- mean sample RMS: `189.941546493408`
- median sample RMS: `140.72427292261332`

Percentage perspective:

- mean sample MAE: `0.0293%` of full `u16` scale
- median sample MAE: `0.0229%`
- mean sample RMS: `0.2899%`
- median sample RMS: `0.2147%`
- signed chroma bias: effectively zero

#### Random matched-field chroma sampling: 40 MHz WCBS clip

Results:

- sampled fields: `43`
- matched fields total: `3031`
- negative mean count: `18`
- positive mean count: `25`
- zero mean count: `0`
- mean of sample means: `+0.0009704952158878563`
- median of sample means: `+0.0010720088440729636`
- mean sample MAE: `77.11347696641755`
- median sample MAE: `21.433091573005477`
- mean sample RMS: `330.4853930641056`
- median sample RMS: `238.6867475188943`

Percentage perspective:

- mean sample MAE: `0.1177%` of full `u16` scale
- median sample MAE: `0.0327%`
- mean sample RMS: `0.5043%`
- median sample RMS: `0.3643%`
- signed chroma bias: effectively zero

#### Current interpretation

- Chroma looks fine on both samples.
- Luma does **not**.
- The earlier `cmp100a` live-validation result did not protect the final
  full-output inspection files from a systematic darker luma result.
- So the next debugging step must use kernel/live-field dumps to locate the
  first stage where the darker luma appears in the integrated output path.

### Important correction: the old Python live-output oracle was wrong for luma

While chasing the native and 40 MHz finished-output dimness, I discovered that
the earlier `python_live_capture` oracle used for `cmp100a` was not actually
mirroring the real `vhs-decode` CLI output path on luma.

Evidence:

- C++ native `inspect` output matches `cmp100a` exactly over the first 99 fields:
  - luma diff = `0`
  - chroma diff = `0`
  - metadata diff = `0`
- but `vhs-decode` CLI output for the same native input differs from the old
  helper-generated `python_live_capture` by about `+3001` luma codes while
  keeping identical metadata:
  - luma mean signed `inspect - livecmp = +3001.151205991727`
  - luma RMS `3006.1500613463527`
  - chroma stayed close:
    - mean `-0.00029770609618518364`
    - RMS `96.95426315701418`

I also ruled out the easy excuses:

- this is **not** a threading artifact in upstream
- `vhs-decode` CLI default threading and `-t 1` produced identical TBC output
- the helper output still stayed darker than the real CLI output by `~3086`
  luma codes even against a fresh `-t 1` CLI run

So the problem is the helper itself: it was instantiating `VHSDecode` with a
reduced/simplified option set instead of mirroring the real CLI construction
path. That means the earlier `cmp100a` luma comfort was against the wrong
upstream oracle.

Implication:

- chroma conclusions from the helper remain broadly plausible
- luma conclusions that depended on the old helper need to be re-validated
  against real `vhs-decode` CLI-equivalent output before treating them as done

### Native luma offset source identified and fixed

This one finally reduced to a concrete live-path bug instead of vague
"integration drift".

#### What the field-matched oracle showed

Using the fixed CLI-faithful Python live oracle for the first written native
field at `fileLoc=583680`:

- pre-resync live luma substrate matches:
  - `cpp_window_video.f64` vs `python_demod.f64`
  - bias `-0.04976502086796034`
  - RMS `0.23513898974734795`
  - corr `0.9999999999993066`

- but the temporary resync-mutated buffer does **not**:
  - `cpp_window_video_resync.f64` vs `python_demod.f64`
  - bias `-60211.45608484165`
  - RMS `60211.45608528023`
  - corr `0.9999999999993069`

- and the final emitted luma inherited that dark shift before the fix:
  - `cpp_luma_float.f32` vs `python_luma_float.f64`
  - bias `-56756.77742656583`
  - RMS `56802.6727767067`
  - corr `0.9999294029060212`
  - `cpp_luma_u16.u16` vs `python_luma_u16.u16`
  - bias `-2991.3514227217647`
  - RMS `2994.0267641137257`
  - corr `0.99992724718947`

That makes the source very clear:

- the live decoder's field/output path should use the original demodcache
  `video["demod"]` substrate for luma downscale
- **not** the temporary `fin.demod` buffer after Resync/get_pulses mutates it
  while searching sync

#### Root cause in the C++ driver

In [cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp),
the live driver was feeding:

- `fin.demod`

into K4/downscale for luma.

That looked plausible during earlier bring-up because geometry stayed good, but
it is not faithful to upstream. The upstream field object's stored
`data["video"]["demod"]` on the first written native field matches the original
pre-resync cache window, not the resync-mutated temporary buffer.

So the fix was:

- switch live luma K4 input back to `window->video`
- keep the resync-mutated `fin.demod` only for the sync-finding path

#### Immediate numeric effect of the fix

After switching luma K4 input from `fin.demod` back to `window->video` on that
same first written field:

- `cpp_luma_u16.u16` vs `python_luma_u16.u16`
  - bias `+42.98893577904985`
  - RMS `133.17523736568802`
  - corr `0.999928968556875`

So the first-field dark bias collapsed from about `-2991` codes to about `+43`
codes.

#### Broad native recheck against the finished `vhs-decode` minute output

Re-ran the native C++ decoder over the first `110` seen fields and compared the
first `56` exact matched written fields against:

- [TAPE_1_1min_33pct_vhsdecode_inspect.tbc](/media/hunter/DATA/captures/TAPE_1_1min_33pct_vhsdecode_inspect.tbc)
- [TAPE_1_1min_33pct_vhsdecode_inspect_chroma.tbc](/media/hunter/DATA/captures/TAPE_1_1min_33pct_vhsdecode_inspect_chroma.tbc)

Results:

- luma:
  - bias `-57.84626815335669`
  - RMS `271.03670574162794`
  - corr `0.999674051466832`
  - `0.9809136400779593%` of upstream luma signal RMS

- chroma:
  - bias `+0.0007256865893476431`
  - RMS `247.9605638267263`
  - corr `0.9926807888036242`
  - `0.7552777563329481%` of upstream chroma signal RMS

So the systematic native full-output dark-luma problem is no longer a mystery:

- source identified
- source fixed
- broad recheck no longer shows the old `~ -3000` code native luma bias

#### Remaining follow-up

- Repeat the same post-fix full-output check on the `40 MHz` finished
  inspection output.
- Then return to the local bad-field handling around the user's reported
  `40 MHz` field `35` issue.

### 40 MHz damaged-window fixes 1-5

After the broad native luma issue was fixed, the remaining visual problem was a
localized damaged stretch on the true `40 MHz` WCBS clip around fields
`~20-40`, with the user's worst example around field `35`.

Important framing:

- This work was done against:
  - [wcbs_40m_vhsdecode_inspect.tbc](/media/hunter/DATA/captures/wcbs_40m_vhsdecode_inspect.tbc)
  - [wcbs_40m_vhsdecode_inspect_chroma.tbc](/media/hunter/DATA/captures/wcbs_40m_vhsdecode_inspect_chroma.tbc)
  - [wcbs_40m_vhsdecode_inspect.tbc.json](/media/hunter/DATA/captures/wcbs_40m_vhsdecode_inspect.tbc.json)
- The user's visual impression on the damaged stretch was useful. In this
  region, exact `fileLoc` fidelity and "looks better by eye" were not always
  the same thing.

#### Fix1: stale serration/field-state refresh bug

Changed:

- [cpp_port/src/resync_runtime.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/resync_runtime.cpp)

Root cause:

- `add_pulselevels_to_serration_measures(...)` was bailing out early once
  serration levels existed.
- Upstream does not stop there; it uses the existing levels to re-find pulses
  and refresh field-state.
- Carrying stale serration levels forward was poisoning damaged later fields.

Result:

- `fix1` was a real improvement over the old broken state.
- On exact matched field `35` versus upstream:
  - luma mean `-531.3435173191828`
  - luma MAE `2380.051953369824`
  - luma RMS `4376.010377569237`
  - luma corr `0.9341963610404892`

Artifacts:

- [wcbs_40m_field35_probe_fix1.tbc](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix1.tbc)
- [wcbs_40m_field35_probe_fix1_chroma.tbc](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix1_chroma.tbc)
- [wcbs_40m_field35_probe_fix1.tbc.json](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix1.tbc.json)

Important caveat:

- `fix1` also produced a duplicate/recovery event just before the damaged
  stretch:
  - field `33`: `fileLoc=137871360`, `seqNo=31`, `phase=3`
  - field `34`: `fileLoc=139868160`, `seqNo=33`, `syncConf=0`,
    `decodeFaults=4`
- So `fix1` looked very good by eye in that region, but it was not in perfect
  session/file-location lockstep with upstream.

#### Fix2: field-order confidence override on damaged fields

Changed:

- [cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)

Root cause:

- The live driver was forcing `field_order_confidence = 0`.
- That allowed weak damaged-field evidence to override the normal previous-field
  cadence.
- The result was a misclassified field at `readloc=139223040`, which then
  triggered the duplicate/recovery event before field `35`.

Patch:

- Set `first.field_order_confidence = 100`.

Result:

- The duplicate/recovery event disappeared.
- Exact metadata alignment around fields `33-36` matched upstream:
  - `fileLoc`
  - `isFirstField`
  - `fieldPhaseID`
  - `syncConf`
- This produced the numerically best faithful alignment line.

Exact matched fields `33-36` versus upstream:

- field `33`
  - luma RMS `2048.8914450542143`
  - corr `0.9934839706633681`
- field `34`
  - luma RMS `3265.7821038198826`
  - corr `0.9944418666650763`
- field `35`
  - luma RMS `889.9657494335584`
  - corr `0.9984829121114176`
- field `36`
  - luma RMS `2086.0969505688186`
  - corr `0.9970512526667367`

Artifacts:

- [wcbs_40m_field34_probe_fix2.tbc](/media/hunter/DATA/captures/wcbs_40m_field34_probe_fix2.tbc)
- [wcbs_40m_field34_probe_fix2_chroma.tbc](/media/hunter/DATA/captures/wcbs_40m_field34_probe_fix2_chroma.tbc)
- [wcbs_40m_field34_probe_fix2.tbc.json](/media/hunter/DATA/captures/wcbs_40m_field34_probe_fix2.tbc.json)

#### Fix2fresh

Purpose:

- regenerate the `fix2` line with a fresh field-35 debug capture so the field
  stage dumps were not polluted by earlier intermediate experiments.

Result:

- numerically identical to `fix2` on the damaged window
- useful mainly because it confirmed:
  - field `35` geometry on the aligned `fix2` line was actually very good:
    - `first_hsync_loc`: C++ `52556.01722809606`
    - Python `52554.55913449716`
    - `linelocs_final` RMS `1.5543227869600407`

Artifacts:

- [wcbs_40m_field35_probe_fix2fresh.tbc](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix2fresh.tbc)
- [wcbs_40m_field35_probe_fix2fresh_chroma.tbc](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix2fresh_chroma.tbc)
- [wcbs_40m_field35_probe_fix2fresh.tbc.json](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix2fresh.tbc.json)

#### Fix3 / Fix4: raw-block alignment experiment

Changed:

- [cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)

Hypothesis:

- maybe the damaged-window mismatch came from assembling one fewer demod-cache
  block than upstream (`952320` vs `983040` samples)

Patch:

- try aligning `decodefield()` reads on raw `blocklen` instead of the cut cache
  stride

What actually happened:

- this did produce the larger `983040` local window
- but it also shifted metadata/file position off by one cut block and badly
  regressed the damaged stretch

Example:

- field `34` moved from upstream-aligned `fileLoc=139868160` to `139837440`
- field `36` moved from `141219840` to `141189120`
- field `35` luma RMS blew back up to `8032.412705149451`

Conclusion:

- this branch was a real regression
- not kept

Artifacts:

- [wcbs_40m_field34_probe_fix3.tbc](/media/hunter/DATA/captures/wcbs_40m_field34_probe_fix3.tbc)
- [wcbs_40m_field34_probe_fix3_chroma.tbc](/media/hunter/DATA/captures/wcbs_40m_field34_probe_fix3_chroma.tbc)
- [wcbs_40m_field34_probe_fix3.tbc.json](/media/hunter/DATA/captures/wcbs_40m_field34_probe_fix3.tbc.json)
- [wcbs_40m_field34_probe_fix4.tbc](/media/hunter/DATA/captures/wcbs_40m_field34_probe_fix4.tbc)
- [wcbs_40m_field34_probe_fix4_chroma.tbc](/media/hunter/DATA/captures/wcbs_40m_field34_probe_fix4_chroma.tbc)
- [wcbs_40m_field34_probe_fix4.tbc.json](/media/hunter/DATA/captures/wcbs_40m_field34_probe_fix4.tbc.json)

#### Fix5: use established output levels for current field

Changed:

- [cpp_port/src/ntsc_vhs_decode_main.cpp](/media/hunter/DATA/GitHub/VHSpp/cpp_port/src/ntsc_vhs_decode_main.cpp)

Hypothesis:

- upstream applies live level changes after downscale and only for later fields
- our driver was self-recalibrating the current damaged field's output levels

Patch:

- current field used previously established output levels
- newly detected levels were saved only for later fields

Result:

- numerically this was a real improvement over `fix2fresh` on exact matched
  fields `33-36`

Exact matched fields `33-36` versus upstream:

- field `33`
  - luma RMS `1559.4337228610657`
- field `34`
  - luma RMS `1061.8678916535623`
- field `35`
  - luma RMS `654.869213747315`
- field `36`
  - luma RMS `933.0109733863168`

However:

- the user judged `fix5` to still be a visual regression relative to `fix1`
  on the damaged stretch

So while `fix5` improved exact matched numeric fidelity relative to
`vhs-decode`, it was not kept as the chosen branch for visual quality.

Artifacts:

- [wcbs_40m_field35_probe_fix5.tbc](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix5.tbc)
- [wcbs_40m_field35_probe_fix5_chroma.tbc](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix5_chroma.tbc)
- [wcbs_40m_field35_probe_fix5.tbc.json](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix5.tbc.json)

#### Decision

The codebase was reverted to the exact `fix1` behavior because that was the
preferred visual result on the damaged `40 MHz` stretch.

This was verified by rerunning the decode under a fresh output name and hashing
the results:

- [wcbs_40m_field35_probe_fix1_repro.tbc](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix1_repro.tbc)
- [wcbs_40m_field35_probe_fix1_repro_chroma.tbc](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix1_repro_chroma.tbc)
- [wcbs_40m_field35_probe_fix1_repro.tbc.json](/media/hunter/DATA/captures/wcbs_40m_field35_probe_fix1_repro.tbc.json)

Byte-for-byte identity:

- `.tbc`: identical
- `_chroma.tbc`: identical
- `.tbc.json`: identical

SHA-256:

- original `.tbc`:
  - `2e9732af3a91e6020ff08e47226a10eaf140c1bf0da5a359ea81e156b5831e22`
- repro `.tbc`:
  - `2e9732af3a91e6020ff08e47226a10eaf140c1bf0da5a359ea81e156b5831e22`

- original `_chroma.tbc`:
  - `3bdb3395586bedf0e7b678651d137eb178c7b467bbec9cfcb296b678c512a3de`
- repro `_chroma.tbc`:
  - `3bdb3395586bedf0e7b678651d137eb178c7b467bbec9cfcb296b678c512a3de`

- original `.tbc.json`:
  - `219fe965fc303ead1a29eb73fb1dde23688b4dcdabf19c7c582037f64c2a4cbb`
- repro `.tbc.json`:
  - `219fe965fc303ead1a29eb73fb1dde23688b4dcdabf19c7c582037f64c2a4cbb`

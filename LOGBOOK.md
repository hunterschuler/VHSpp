# VHSpp Logbook

> Development scratchpad, same as cuVHS's LOGBOOK.md.

## Architecture Design (2026-03-26)

### What is VHSpp

CPU-only C++ port of cuVHS. Same DSP algorithms and calibration constants,
but with an architecture designed for multi-core CPUs rather than ported from
the GPU batching model.

### Why not just strip CUDA from cuVHS

cuVHS's architecture (batch all fields through K1, barrier, batch K2, barrier,
etc.) exists because GPUs need it: kernel launch overhead, cuFFT batched plans,
PCIe transfer amortization. None of these apply on CPU. Copying that architecture
would mean synchronization barriers at every kernel stage and a serial prescan
pass — both unnecessary costs on CPU.

### Parallelization: B+overlap with supervisor/worker model

Based on discussion with namazso (vhs-decode developer): the only true cross-field
dependency in sync finding is the flywheel fallback when syncs are too degraded
to find independently. Good syncs are self-describing.

**Worker threads** (N = core count):
- Each owns a chunk of the input file (with overlap regions at boundaries)
- Runs the full K1–K7 pipeline independently, field by field
- Owns its own FFTW plans and scratch buffers — zero sharing, zero locking
- Writes TBC output via `pwrite()` at computed byte offsets as fields complete

**Supervisor thread**:
- Assigns chunks to workers
- Monitors overlap regions: workers report sync positions for overlap fields
- Resolves stitch points: matching VSYNCs found independently by adjacent chunks
- Flywheel fallback: re-processes boundary serially if no sync match found
- Writes JSON metadata (globally ordered)
- Enforces field parity across chunk boundaries
- Reports progress

**Overlap size**: ~30 fields (~0.5 seconds). Provides:
- Lineloc coherence window (±10 fields)
- Chroma track auto-detection context
- NTSC 8-field phase cycle
- Enough clean signal for sync matching even near tape edit points

**Stitching**: Both chunks independently find VSYNC positions in the overlap.
A match = same VSYNC within ±500 samples. Supervisor decides: chunk A owns
fields up to match, chunk B owns everything after.

**Fallback**: If no matching syncs in overlap (tape edit on chunk boundary),
supervisor re-processes the boundary region serially using the previous chunk's
last known good state. Expected to be rare.

### Design constraint: orchestrator-agnostic pipeline stages

Pipeline stage functions (fm_demod, sync_pulses, line_locs, etc.) must be pure
single-field transforms: `(input, state) → output`. No awareness of threading,
chunk boundaries, field ordering, or scheduling strategy.

State structs (FMDemodState, ChromaState, etc.) are owned and carried by the
caller — not global, not singleton. This keeps the door open for alternative
orchestration strategies without touching DSP code:

- **B+overlap** (file mode): supervisor divides file into overlapping chunks,
  workers process chunks independently
- **C / work queue** (live/streaming mode): fields arrive continuously,
  workers pull from shared queue, sequential field ordering
- **Single-threaded** (simple/debug mode): one loop calling stages in order

The orchestrator is the only component that knows the strategy. Stages don't.

### Why this is better than GPU-style batching on CPU

| Aspect                | GPU batching (cuVHS) | B+overlap (VHSpp) |
|-----------------------|---------------------|--------------------|
| Serial prescan        | Required (12-53s)   | Eliminated         |
| Cross-kernel barriers | Every stage          | None               |
| Cache behavior        | Bad (field data evicted between stages) | Good (one field stays hot through K1-K7) |
| Worker independence   | All share batch state | Fully independent  |
| Complexity            | Simpler              | Medium             |

### Memory management

**Per-worker scratch**: ~26 MB (FFTW plans + FFT buffers + demod/sync/lineloc
arrays for one field). Total: `num_workers × 26 MB`. On 8 cores = ~200 MB.

**No batch buffering**: Workers write each field to disk immediately via
`pwrite()`. RAM usage is just the per-worker scratch, not proportional to
file size.

**FFTW plans**: Created once per worker at init. `fftw_execute_dft_r2c(plan,
different_in, different_out)` is thread-safe per FFTW docs. Plan creation is
NOT thread-safe, so each worker creates its own plans.

**Cache locality**: Processing one field end-to-end (K1→K7) keeps its ~3.6 MB
of data hot in L2/L3. This is the primary performance advantage of per-field
processing over batch-per-kernel.

### FFT: FFTW3

cuFFT API mapping:
- `cufftExecD2Z` → `fftw_execute_dft_r2c`
- `cufftExecZ2D` → `fftw_execute_dft_c2r`
- `cufftExecZ2Z` → `fftw_execute_dft`
- `cufftDoubleComplex` → `fftw_complex` (`double[2]`)

Neither cuFFT nor FFTW normalize — cuVHS pre-bakes 1/N into filter coefficients.
Same trick works with FFTW, so filter init code ports directly.

FFT sizes: pad to 7-smooth (factors of 2,3,5,7) for efficient transforms.
Same logic as cuVHS.

### What ports verbatim from cuVHS

All DSP algorithms and calibration constants:
- Butterworth filter magnitudes (bilinear transform formula)
- Shelf filter (deemphasis), supergaussian LPF, FIR lowpass
- Phase unwrap (ediff1d → unwrap → clamp [0,2π] → scale)
- Adaptive pulse classification (measure median HSYNC, shift thresholds)
- VBLANK state machine (HSYNC → EQ1 → VSYNC → EQ2 → HSYNC)
- Right-edge hsync refinement (less susceptible to FM overshoot)
- Catmull-Rom cubic interpolation for TBC resample
- Continuous heterodyne phase (absolute sample position, not per-line reset)
- Chroma pre-bandpass (60 kHz – 1.2 MHz, prevents luma→chroma contamination)
- Burst cancellation metric for track auto-detection
- Dropout detection with hysteresis + merge + concealment
- All VideoFormat constants (IRE mapping, pulse widths, output scaling)

### Format compatibility: NTSC-only for now (2026-03-26)

**NTSC VHS SP is the only supported system.** PAL constants existed in cuVHS
(sourced from vhsdecode's `format_defs/vhs.py`) but were never tested — cuVHS
never decoded a PAL tape. We've stripped them from VHSpp to avoid shipping
unverified code disguised as support.

**Strategy for future PAL/other format support:**
- Every NTSC-specific decision is tagged `[NTSC-SPECIFIC]` in the code
- `grep -r NTSC-SPECIFIC src/` gives you the full audit list
- `video_format.cpp` constructor rejects non-NTSC with a clear error
- When ready: add an else-if branch in the constructor, validate with real
  PAL captures, and grep the tag to find every pipeline decision that needs
  a system-specific path (active video region, field phase IDs, chroma
  decode phase sequence, burst reference, etc.)

### Key cuVHS lessons to preserve (not repeat mistakes)

- `ref_line = 19` aligns output with Python vhs-decode
- `right_edge_offset = 2.25 * (sample_rate_mhz / 40.0)` — empirical constant
- u8 input: normalize as `(sample - 128) * 256` — prevents chroma amplitude bug
- Without chroma pre-bandpass, luma FM carrier heterodynes into fsc band → cycling color
- Field parity from VSYNC pulse spacing, not naive `i%2` — tape edits break alternation
- Adaptive thresholds: different VCRs produce different pulse widths (±2% typical)
- FFT size 468140 has prime factor 23407 → terrible FFT perf. Pad to 468750 (7-smooth)
- K3 VSYNC state machine can lock onto next field's VSYNC near tape edits
  (ref_pulse_idx in second half of pulse list). Fix: subtract samples_per_field.
  CRITICAL: guard `corrected > 0` — field 0's VSYNC is legitimately mid-list.
  Without the guard, field 0 goes negative → corrupts all downstream state.

### Prescan bug fixes from cuVHS (2026-03-26)

VHSpp doesn't use a prescan (B+overlap eliminates it for file mode), but these
apply if we ever add a scan pass for streaming or diagnostics.

1. **long_win too large for short files**: `long_win = spf * 50` (~24M samples
   at 50 MHz). For a 14-field test file the window exceeded the file, so the
   envelope detector never fired. Fix: `long_win = min(spf * 50, total_samples / 2)`.

2. **Prescan was u8-only**: Allocated `uint8_t*` and indexed each byte as one
   sample. For s16 (2 bytes/sample) every other byte is a high byte → garbage
   envelope. Fix: use `double*` buffer via `reader.read_at()` (format-agnostic
   conversion). Add `fabs()` pass for signed formats.

3. **Chunk size was memory-fixed, not sample-fixed**: 256 MB chunk assumed
   1 byte/sample. Fix: express as `256 MB / sizeof(double)` samples (~32M).
   Buffer is always 256 MB of doubles regardless of input format. Fewer samples
   per chunk than the old u8 path but overhead is negligible (~80ms of window
   re-init for a 2-hour tape).

**Key insight**: prescan should work on converted doubles, not raw bytes. Makes
format support automatic rather than per-format envelope code.

### Sample rate discovery: 28.636 MHz (8×fsc), not 28.0 MHz (2026-03-27)

SDR captures of NTSC VHS are at **28.636 MHz** (8× the color subcarrier,
3.579545 MHz). We defaulted to 28.0 MHz, which is a 2.3% error. This silently
broke nearly everything downstream because the sample rate feeds into all
frequency-domain computations.

**How we found it**: hsync_refine produced shift=0.0 for every line — it
couldn't find any HSYNC edges. Measured HSYNC-to-HSYNC spacing was 1820
samples. At 28.0 MHz that implies a 65.0 µs line period (NTSC is 63.556 µs).
But 1820 × 15734.264 Hz = 28.636 MHz exactly — the standard 8×fsc SDR rate.

**What broke at 28.0 MHz**:
- All frequency-domain filters shifted ~2.3% (RF bandpass, deemphasis, video
  LPF, sync FIR) — cutoffs computed relative to sample rate
- IRE-to-Hz constants wrong — ire0, hz_ire, sync threshold all derived from
  sample rate, causing incorrect luma scaling (dark image)
- HSYNC refinement dead — search windows sized in µs×sample_rate missed every
  edge, producing zero corrections and diagonal tearing in output
- Chroma color-under frequency off — heterodyne phase accumulator drifts
  because carrier frequency is wrong for the actual rate

**Why cuVHS didn't catch it**: cuVHS also defaults to 28.0 MHz and the error
was never noticed because the GPU pipeline was validated against vhs-decode
Python output which uses the correct rate. The cuVHS default should also be
fixed.

**Fix**: Default to 28.636 MHz. Auto-detect from file extension if possible.
The `--rate` flag remains for non-standard captures.

### VSYNC drift and field alignment tearing (2026-03-27)

**Problem**: Semi-regular tearing and color corruption (green/red/magenta banding)
at intervals of ~150 fields. Worst-case frames had the bottom 60% of the image
corrupted with wild chroma. Initially suspected head-switch transients, but the
corruption was far too large (head-switch affects ~6-8 lines, not half the frame)
and the interval was semi-regular rather than every field.

**Root cause**: The sequential pipeline advanced `file_offset += spf` (fixed
nominal samples-per-field) after each field. But actual VHS fields are slightly
shorter than nominal — the VSYNC drifted ~900 samples/field relative to the read
window. After ~150 fields, the VSYNC fell off the front of the buffer
(`linelocs[0]` went negative). At that point, `line_locs` grabbed the NEXT
field's VSYNC instead, producing garbage alignment for the current field. The
cycle then repeated as the VSYNC wrapped around.

**Diagnosis**: Instrumented pipeline to print `linelocs[0]` for every field:
```
field   0: ll0=135863 (0.284 * spf)
field  99: ll0= 45500 (0.095 * spf)   ← drifting toward zero
field 152: ll0= -1362 (-0.003 * spf)  ← fell off the buffer!
field 199: ll0=433715 (0.906 * spf)   ← wrapped to next field's VSYNC
```

**What we tried**:
1. `file_offset += linelocs[0]` — Advance to VSYNC position. Failed: only
   advanced ~28% of a field per iteration, producing infinite fields (6000+ and
   counting). The VSYNC landed at position 0, line 0 mapped to negative position,
   and the pipeline got stuck re-decoding the same data.

2. Proportional drift correction (what worked): Target a stable VSYNC position
   at `spf * 0.25` in the buffer. Apply correction:
   `advance = spf + (linelocs[0] - target)`. This converges in ~1 iteration.
   If ll0 is ahead of target, advance more (pushing it back); if behind, advance
   less (pulling it forward). Sanity-clamped to `[0.5*spf, 1.5*spf]` with
   fallback to fixed `spf`.

**Result**: Tearing eliminated across 500+ frames of 1-minute test decode. Frame
374 (previously worst-case with full-frame color corruption) is now clean. Field
count stable at ~3596 (vs 3589 fixed, 3592 vhs-decode reference).

**Why cuVHS doesn't have this problem**: cuVHS uses a prescan pass that finds
exact VSYNC positions before decoding. Each field's read window is positioned
precisely around its known VSYNC. The sequential VHSpp pipeline discovers VSYNC
positions on-the-fly, so it needs this feedback loop to stay aligned.

### FM demod phase unwrap boundary fix (2026-03-27)

**Problem**: Tearing and black streaks scanning down the image at head-switch
positions. The sequential pipeline demodulated two independent `spf`-sized halves
of the `2*spf` read window. Each half reset its phase unwrap state, creating a
discontinuity at the boundary: `demod[spf] = 0`. Output lines crossing the spf
boundary got corrupted interpolation. As `file_offset` advanced per field, the
affected line shifted position → "scanning down" visual artifact.

**Fix**: Initialize FM demod state with `target_samples = 2*spf` (larger FFT
plans and scratch buffers). Process the full `2*spf` buffer in a single
`fm_demod()` call with continuous phase unwrap. Added `target_samples` parameter
to `FMDemodState::init()`.

**Side effect**: Single-threaded performance improved from ~7 fps to ~13 fps.
One large FFT is more efficient than two smaller ones, and the overhead of
double initialization/teardown is eliminated.

### Chroma fixes (2026-03-27)

**het_offset always 0**: VHSpp was passing `chroma_state.cycle_start`
(incrementing 0,1,2,3,0,...) as the heterodyne phase offset. cuVHS uses 0 for
all fields — the phase offset is always 0, only track (0/1) alternates. Fixed.

**Burst cancellation metric**: VHSpp measured per-line burst RMS. cuVHS sums
adjacent line pairs `fabs(a + b)` — NTSC burst alternates 180° per line, so
correct track cancels (low metric), wrong track adds (high metric). Rewrote to
match cuVHS's adjacent-pair approach. Track detection immediately flipped from
wrong to correct, restoring color.

### Notes from cuVHS worker (2026-03-27)

**Sliding window cap for short files**: If any future prescan or analysis uses a
long sliding window (e.g. `long_win = spf * 50`), cap at `min(spf * 50,
total_samples / 2)`. cuVHS hit this on a 28-field color bar test file — window
exceeded the buffer. Already noted in prescan bug fixes above but worth
reinforcing: applies to any sliding window, not just prescan.

**Pre-demod field detection is fundamentally fragile** (updated 2026-03-27):
All RF captures are AC-coupled — the capture device always has a blocking
capacitor. There are no "DC-coupled" captures. The cuVHS prescan detects head
switch transients: the two VHS heads have different DC offsets, creating a
~25/30 Hz square wave. The blocking cap converts each edge into a transient
spike that the moving-mean envelope detector finds (essentially a crude LPF
zero-crossing detector for head switch harmonics, per namazso's analysis).

This is fragile because it relies on a side-effect that depends on VCR circuit
topology. Some VCRs (e.g., Sony SLV-778HF) have active components that filter
out the head switch transient before it reaches the RF test point. The Rigol
capture failure wasn't an AC coupling issue — the information was gone before
it left the VCR.

cuVHS plans to fix this by demodding first in large GPU chunks, then finding
actual VSYNC in the clean baseband signal — which is exactly what VHSpp already
does. Our post-demod field detection (sync_pulses + line_locs on demod05) is
immune to this issue by design.

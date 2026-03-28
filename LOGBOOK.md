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

### Parallelization: chunkd overlap with supervisor/worker model

Based on discussion with namazso (vhs-decode developer): the only true cross-field
dependency in sync finding is the flywheel fallback when syncs are too degraded
to find independently. Good syncs are self-describing. namazso independently
arrived at the same chunkd overlap approach for vhs-decode-lite (Python),
confirming this is the right architecture for CPU decoders.

**Core concept**: Divide input into overlapping chunks (chunks). Each worker
processes its chunk independently — full K1–K7 pipeline, including VSYNC
discovery via the drift-corrected loop. Supervisor assigns chunks, stitches
output at boundaries, and handles tape edit cleanup.

**Two modes, one architecture**: Async (file) and live (streaming) use the same
worker code, same stitching logic, same cleanup mechanism. The only difference
is chunk sizing:

#### Async mode (file decode)

Goal: maximum throughput.

```
File:     [========================================================]
Chunks:  [====S1====][====S2====][====S3====][====S4====][====S5====]
                  ↕overlap    ↕overlap    ↕overlap    ↕overlap
Workers:  W1:S1      W2:S2      W3:S3      W4:S4      W5:S5  ...
          (all run in parallel)
Output:   [=F1=][=F2=][=F3=][=F4=][=F5=] → stitch → final TBC
```

- Divide file into N large chunks (file_size / num_workers), with overlap
  regions (~30 fields / ~0.5s) at boundaries
- All workers launch simultaneously, process chunks in parallel
- Each worker runs the drift-corrected sequential loop on its chunk
- Workers write TBC fields via `pwrite()` at computed byte offsets
- Supervisor stitches at boundaries when workers complete
- Tape edit cleanup pass runs after all workers finish (see below)
- Chunk size: large (total_size / num_workers). Minimizes overlap waste.

#### Live mode (streaming during capture)

Goal: low latency with full fidelity.

```
Capture:  ████████████████████████████████████████████►
Chunks:       [S1][S2][S3][S4][S5][S6][S7]...
                ↕ov ↕ov ↕ov ↕ov ↕ov ↕ov
Workers:       W1  W2  W3  W1  W4  W2  ...
               (assigned as chunks fill)
Output:          ██  ██  ██  ██  ██  ██ ...
                 ↑
            first output at ~5-10s
```

- Small chunks (~2-5 seconds) assigned to workers as capture data fills
- Same worker code, same stitching — just smaller pieces
- Latency = chunk_capture_time + one_chunk_processing_time ≈ 5-10 seconds
- Overlap waste is high but irrelevant: compute budget vastly exceeds capture
  rate (e.g., 6 cores × 13 fps = 78 fps vs 30 fps capture)
- Tape edit handling: buffer + deferred cleanup (see below)

#### Workers

**Worker threads** (N = detected core count, platform-adaptive):
- Each owns a chunk of the input (with overlap at boundaries)
- Runs the full K1–K7 pipeline independently, field by field
- Uses VSYNC drift-corrected advancement (proportional feedback loop)
- Owns its own FFTW plans and scratch buffers — zero sharing, zero locking
- Reports: first/last valid VSYNC positions, field count, chroma state at
  chunk boundaries

**Core count detection**: Platform-dependent syscalls required.
- Linux: `sysconf(_SC_NPROCESSORS_ONLN)` or `std::thread::hardware_concurrency()`
- macOS: `sysctl hw.ncpu`
- Windows: `GetSystemInfo()` → `dwNumberOfProcessors`
- Fallback: `std::thread::hardware_concurrency()` (portable but not always
  accurate). Reserve 1 core for supervisor. Minimum 6 cores recommended.

#### Supervisor

- Assigns chunks to workers (upfront for async, on-demand for live)
- Stitches output at chunk boundaries: match VSYNCs in overlap within ±500
  samples. Both workers produce fields for the overlap; supervisor picks a
  cut point and discards duplicates.
- Writes globally-ordered JSON metadata
- Enforces field parity across boundaries
- Reports progress
- Manages tape edit cleanup (see below)

#### Tape edit handling

Tape edits at chunk boundaries are not rare enough to ignore. On a 2-hour tape
with 30s chunks, ~240 boundaries. With ~5 edits per tape, probability of at
least one boundary landing on an edit is significant. Edits can be 50ms to 4s.

**Requirements**: Must preserve timing (audio sync). Must decode the edit region
(fidelity — never replace real signal with blanks). Small cleanup cost acceptable.

**Async mode**: Cleanup pass runs after all workers complete.
1. Worker A reports last valid field position before the edit
2. Worker B reports first valid field position after the edit
3. Supervisor identifies the gap
4. One worker re-decodes just that region serially, using worker A's last known
   chroma state as initial conditions
5. Cleanup output fills the gap in the final TBC
6. Cost: ~1 second wall time per edit boundary. Negligible.

**Live mode**: Same cleanup, triggered inline.
1. Failed stitch detected → buffer output for that region, don't emit yet
2. Keep processing subsequent chunks normally with remaining workers
3. When stitching succeeds again (far side of edit): extent of edit is known
4. One worker enters "penalty box" — runs cleanup pass on the buffered region
5. Cleaned-up output emitted, worker rejoins live pool
6. Latency penalty only during/after the edit. Normal playback unaffected.
7. At 6+ cores, losing one worker temporarily is fine:
   5 workers × 13 fps = 65 fps, still 2× capture rate.
8. Degenerate case (tape that's mostly edits): detect when too many workers
   are in cleanup simultaneously → warn user, suggest async mode.

#### Overlap sizing

~30 fields (~0.5 seconds) per chunk boundary. Provides:
- VSYNC matching context for stitching
- Lineloc coherence window (±10 fields)
- Chroma track auto-detection context
- NTSC 8-field phase cycle
- Enough clean signal for sync matching even near tape edit points

#### Chunk sizing tradeoffs

| Chunk size | Overlap waste | Latency  | Use case        |
|-------------|---------------|----------|-----------------|
| Large (30s+)| ~1-2%         | N/A      | Async file mode |
| Medium (5s) | ~10%          | ~8-10s   | Live monitoring |
| Small (2s)  | ~25%          | ~4-5s    | Live preview    |

### Design constraint: orchestrator-agnostic pipeline stages

Pipeline stage functions (fm_demod, sync_pulses, line_locs, etc.) must be pure
single-field transforms: `(input, state) → output`. No awareness of threading,
chunk boundaries, field ordering, or scheduling strategy.

State structs (FMDemodState, ChromaState, etc.) are owned and carried by the
caller — not global, not singleton. This keeps the door open for alternative
orchestration strategies without touching DSP code.

The orchestrator is the only component that knows the strategy. Stages don't.

### Why this is better than GPU-style batching on CPU

| Aspect                | GPU batching (cuVHS) | Chunkd overlap (VHSpp) |
|-----------------------|---------------------|--------------------------|
| Serial prescan        | Required (12-53s)   | Eliminated               |
| Cross-kernel barriers | Every stage          | None                     |
| Cache behavior        | Bad (field data evicted between stages) | Good (one field stays hot through K1-K7) |
| Worker independence   | All share batch state | Fully independent       |
| Live mode             | Not possible         | Same architecture, small chunks |

### Memory management

**Per-worker scratch**: ~26 MB (FFTW plans + FFT buffers + demod/sync/lineloc
arrays for one field). Total: `num_workers × 26 MB`. On 8 cores = ~200 MB.

**No batch buffering**: Workers write each field to disk immediately via
`pwrite()`. RAM usage is just the per-worker scratch, not proportional to
file size.

**FFTW plans**: Created once per worker at init. `fftw_execute_dft_r2c(plan,
different_in, different_out)` is thread-safe per FFTW docs. Plan creation is
NOT thread-safe, so each worker creates its own plans serially at startup.

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

### Async orchestrator and parallel performance (2026-03-27)

Built the parallel file-mode pipeline:
- **TBCPWriter**: pwrite()-based TBC writer allowing concurrent field output at
  computed byte offsets. Pre-allocates files, supports compaction after stitching.
- **ChunkWorker**: Per-chunk function running full K1–K7 pipeline. Thread-local
  pread() for input. VSYNC-locked advancement. Per-stage profiling (worker 0 only).
- **AsyncOrchestrator**: Divides file into N overlapping chunks, launches workers
  in parallel, monitors progress (atomic counter + 500ms poll), stitches at
  boundaries, compacts output.

**Stitching**: Match absolute VSYNC positions (file_offset + lineloc0) within
±500 sample tolerance. Count ALL consecutive matches at each boundary (not just
the first) to correctly trim duplicate fields. Verified: exact field count match
across all thread counts (1, 4, 8, 12, 23).

**Performance on i9-12900K (8P+8E, 30MB L3)**:
- Sequential: 13 fps (single-threaded pipeline)
- Async 23 threads, full-field FFT: ~35 fps (2.7× speedup, memory-bandwidth limited)
- Async 23 threads, tiled FFT (15K tiles): ~110 fps (8.5× speedup, but visual artifacts — see tiling section)
- Profile: FM demod = 52%, chroma decode = 40% of processing time

### Tiled FM demod: attempted, investigated, abandoned (2026-03-27)

**Problem**: 23 workers × 960K-point FFT = ~46 MB working set per worker.
Total ~1 GB vs 30 MB L3 cache. perf stat showed IPC=0.49, ~80% cache miss rate.

**Attempted fix**: Overlap-save tiled FM demod. Cache-adaptive tile sizing from
/sys/devices/system/cpu/cpu0/cache/ (L2/L3 query). Tile size ~15360 (7-smooth),
overlap = 4096 samples. At 80% L2 budget: 1280K L2/core × 0.80 / 68 bytes per
sample ≈ 15K tile. This gave ~85 tiles per 2×spf buffer.

**Performance**: Tiling genuinely helped. Full-field async = ~35 fps. Tiled
async = 110+ fps. The cache-friendly tiles eliminated the L3 thrashing.
(Previous logbook entry incorrectly claimed 110fps without tiling — that was
never tested; the 110fps was always from the tiled path.)

**Visual artifacts**: White "comet" streaks (short bright horizontal streaks
like shooting stars) and mild tearing, appearing ONLY with the tiled path.
Full-field output was clean.

**Attempted fixes** (both failed — comets persisted):

1. **demod_raw buffer separation**: Pass 1 writes atan2 angles + phase unwrap
   to a separate demod_raw[] buffer. Pass 2 reads from demod_raw[] exclusively
   to prevent in-place corruption where one tile's filtered output contaminates
   the next tile's overlap input. Comets persisted.

2. **Sequential phase unwrap**: Moved phase unwrap entirely out of the per-tile
   loop. Pass 1 stores raw atan2 angles in demod_raw[]. A single sequential
   pass converts angles → frequencies over the entire field (no tile boundaries
   in the unwrap at all). Pass 2 does tiled filtering on the unwrapped
   frequencies. Comets STILL persisted.

**Key observations**:
- Artifacts do NOT appear at regular tile-boundary intervals (~6.2 lines/tile),
  ruling out a simple tile-boundary phase discontinuity
- Since the sequential phase unwrap fix didn't help, the root cause is NOT in
  the phase unwrap step
- The remaining suspects are: (a) insufficient overlap for the analytic signal
  Hilbert transform (impulse response decays as 1/n — theoretically infinite),
  (b) the tiled RF bandpass + Hilbert combination producing subtly different
  analytic signal magnitudes/angles vs full-field even in valid regions,
  (c) the 4096-sample overlap being insufficient for one or more of the
  Pass 2 filters (video LPF, deemphasis shelf, sync FIR)
- Envelope scaling: FFTW's unnormalized inverse C2C means tiled envelope
  values are scaled by N_tile (~15K) while full-field values are scaled by
  N_fullfield (~960K) — a ~62× difference. dropout_detect uses 18% of
  field-mean envelope as threshold, so the relative threshold is unaffected,
  but this discrepancy was never verified as harmless
- Never got to the point of running both paths on the same input to diff
  sample-by-sample (the definitive diagnostic)

**Decision**: Abandoned tiling. The artifact is somewhere in the overlap-save
interaction with the Hilbert transform / analytic signal creation, and the fix
is not obvious. Full-field FFT at ~35 fps async is the baseline. Future
optimization should target eliminating unnecessary FFTs (IIR filters for
Pass 2, IIR biquad cascade for chroma bandpass) rather than trying to tile
the existing FFT approach.

### FFTW_MEASURE: tried and reverted (2026-03-27)

Hypothesis: FFTW_ESTIMATE picks a naive FFT algorithm. FFTW_MEASURE benchmarks
alternatives and picks cache-oblivious recursive decompositions that reduce L3
misses. Theory: same FFT, same result, fewer cache misses.

**Result**: 102s planning for worker 0 (first-time measurement of 960K-point
FFTs). Wisdom cached so workers 1-22 planned in 0.3-1.4s each. But steady-state
throughput dropped to **~26 fps** — a 4× regression from 110 fps.

**Why it failed**: FFTW_MEASURE optimizes for single-threaded execution. The
algorithm it chose likely uses wider radixes and larger stride patterns that are
optimal when one FFT owns the entire cache hierarchy, but catastrophic when 23
workers compete for 30MB L3. The "simple heuristic" from FFTW_ESTIMATE happens
to cause less inter-worker interference.

### Cache/bandwidth analysis and future optimization path (2026-03-27)

**Current state**: ~35 fps full-field, 23 threads, i9-12900K. 2.7× scaling on
23 threads (theoretical max: 23×). Bottleneck: DDR memory bandwidth wall
(~25-30 GB/s practical). Each worker's 960K-point FFT touches ~15MB per
operation. 23 workers = ~350MB hot data fighting for 30MB L3.

**What can't be fixed**: Pass 1 of FM demod (RF bandpass + Hilbert transform)
requires a full-field FFT. The Hilbert transform is fundamentally a
frequency-domain operation.

**What can be fixed** — eliminate FFTs that don't need to be FFTs:

1. **FM demod pass 2** (~26% of total time): Video LPF, deemphasis shelf, and
   sync FIR are just filters applied via FFT because cuVHS did it that way for
   the GPU. On CPU, streaming IIR biquads do the same job with ~100 bytes
   working set instead of 15MB.

2. **Chroma bandpass** (~20-30% of total time): 480K-point FFT for a gentle
   order-4 bandpass at 60kHz-1.2MHz. Textbook IIR biquad cascade,
   forward-backward pass (sosfiltfilt) in time domain.

**Estimated impact**: ~50% of FFT work eliminated. Remaining pass 1 FFT still
hits memory bandwidth, but with half the demand, contention drops. Realistic
target: 150-180 fps.

**Beyond that**: would require fundamentally different demod approach
(time-domain I/Q with local oscillator, like SDR) to eliminate the last FFT.

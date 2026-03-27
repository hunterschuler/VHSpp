#include "pipeline/pipeline.h"
#include "pipeline/fm_demod.h"
#include "pipeline/sync_pulses.h"
#include "pipeline/line_locs.h"
#include "pipeline/hsync_refine.h"
#include "pipeline/lineloc_coherence.h"
#include "pipeline/tbc_resample.h"
#include "pipeline/chroma_decode.h"
#include "pipeline/dropout_detect.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>

// ==========================================================================
// Pipeline orchestrator — placeholder single-threaded implementation
//
// TODO: Replace with B+overlap supervisor/worker model.
// This sequential version exists to verify that all pipeline stages compile,
// link, and produce correct output before adding parallelism.
//
// Key insight: raw data doesn't start at a field boundary. The VSYNC can
// be anywhere in a samples_per_field-sized chunk. To get a complete field's
// worth of demod data, we read overlapping 2*spf windows and FM demod
// both halves. This ensures linelocs (which can extend past spf from the
// VSYNC position) still reference valid demod data.
// ==========================================================================

bool Pipeline::run(RawReader& reader, TBCWriter& writer, const VideoFormat& fmt,
                   int num_threads)
{
    (void)num_threads;  // unused in sequential mode

    int spf = fmt.samples_per_field;
    int buf_len = 2 * spf;  // overlapping window

    // Initialize per-worker state (just one "worker" for now)
    FMDemodState fm_state;
    if (!fm_state.init(fmt, buf_len)) {
        fprintf(stderr, "Failed to initialize FM demod\n");
        return false;
    }

    ChromaDemodState chroma_demod;
    if (!chroma_demod.init(fmt)) {
        fprintf(stderr, "Failed to initialize chroma demod\n");
        return false;
    }

    ChromaState chroma_state;

    // Per-field buffers — 2*spf for overlapping window
    std::vector<double> raw(buf_len);
    std::vector<double> demod(buf_len);
    std::vector<double> demod05(buf_len);
    std::vector<double> envelope(buf_len);
    std::vector<double> scratch(buf_len);

    int pulse_starts[MAX_PULSES];
    int pulse_lengths[MAX_PULSES];

    std::vector<double> linelocs(fmt.lines_per_frame);
    std::vector<uint16_t> tbc_luma(fmt.output_field_lines * fmt.output_line_len);
    std::vector<uint16_t> tbc_chroma(fmt.output_field_lines * fmt.output_line_len);

    int fields_done = 0;
    size_t file_offset = 0;  // current read position in samples

    while (true) {
        // Read 2*spf raw samples from overlapping window.
        // File mode: use read_at for random access.
        // Stream mode: use read_next (no overlap recovery possible).
        size_t n;
        if (reader.is_seekable()) {
            n = reader.read_at(raw.data(), file_offset, buf_len);
        } else {
            // Stream mode: can only read forward. First iteration reads
            // 2*spf; subsequent iterations shift and read spf more.
            if (fields_done == 0) {
                n = reader.read_next(raw.data(), buf_len);
            } else {
                // Shift second half to first half, read new second half
                memmove(raw.data(), raw.data() + spf, spf * sizeof(double));
                size_t n2 = reader.read_next(raw.data() + spf, spf);
                n = spf + n2;
                if (n2 == 0) break;
            }
        }

        if (n == 0) break;
        if ((int)n < buf_len) {
            // Zero-pad remainder
            memset(raw.data() + n, 0, (buf_len - n) * sizeof(double));
        }

        // K1: FM demodulation — single pass over full 2*spf buffer.
        // Continuous phase unwrap across the entire window eliminates the
        // boundary discontinuity that caused tearing/head-switch artifacts.
        fm_demod(fm_state, raw.data(), demod.data(), demod05.data(),
                 envelope.data(), buf_len, fmt);

        // K2: Sync pulse detection — only in first spf (this field's syncs)
        int num_pulses = sync_pulses(demod05.data(), pulse_starts, pulse_lengths,
                                     spf, fmt);

        // K3: Line location assignment
        int is_first_field = -1;
        line_locs(pulse_starts, pulse_lengths, num_pulses,
                  linelocs.data(), &is_first_field, spf, fmt);

        // DEBUG: dump diagnostics for first 4 fields
        if (fields_done < 4) {
            double d_min = demod[0], d_max = demod[0];
            int below_count = 0;
            for (int i = 1; i < spf; i++) {
                if (demod[i] < d_min) d_min = demod[i];
                if (demod[i] > d_max) d_max = demod[i];
                if (demod05[i] <= fmt.pulse_threshold_hz) below_count++;
            }
            // Raw input range (post-normalization)
            double r_min = raw[0], r_max = raw[0];
            for (int i = 1; i < spf; i++) {
                if (raw[i] < r_min) r_min = raw[i];
                if (raw[i] > r_max) r_max = raw[i];
            }
            fprintf(stderr, "  [DBG] field %d: pulses=%d is_first=%d\n", fields_done, num_pulses, is_first_field);
            fprintf(stderr, "    raw=[%.1f, %.1f]  demod=[%.0f, %.0f]  below_thr=%.1f%%\n",
                    r_min, r_max, d_min, d_max, 100.0 * below_count / spf);
            fprintf(stderr, "    linelocs: [0]=%.1f [19]=%.1f [262]=%.1f\n",
                    linelocs[0], linelocs[19], linelocs[262]);
        }

        // K3.5: Lineloc coherence (skip for now — needs multi-field context)

        // K4: Hsync refinement — use full 2*spf buffer for bounds
        hsync_refine(demod05.data(), linelocs.data(), buf_len, fmt);

        // K5: TBC resample (luma) — use full 2*spf buffer
        tbc_resample(demod.data(), linelocs.data(), tbc_luma.data(), buf_len, fmt);

        // DEBUG: TBC luma stats for first 4 fields
        if (fields_done < 4) {
            uint16_t lmin = tbc_luma[0], lmax = tbc_luma[0];
            int blank_count = 0;
            int total_px = fmt.output_field_lines * fmt.output_line_len;
            uint16_t blank_val = static_cast<uint16_t>(fmt.output_zero);
            for (int i = 0; i < total_px; i++) {
                if (tbc_luma[i] < lmin) lmin = tbc_luma[i];
                if (tbc_luma[i] > lmax) lmax = tbc_luma[i];
                if (tbc_luma[i] == blank_val) blank_count++;
            }
            fprintf(stderr, "    TBC luma: min=%d max=%d blank(%d)=%d/%d (%.1f%%)\n",
                    lmin, lmax, blank_val, blank_count, total_px, 100.0 * blank_count / total_px);
            for (int ln : {0, 50, 100, 200, 250}) {
                if (ln < fmt.output_field_lines) {
                    uint16_t* row = tbc_luma.data() + ln * fmt.output_line_len;
                    double avg = 0;
                    int start = fmt.output_line_len / 2 - 50;
                    for (int c = start; c < start + 100; c++) avg += row[c];
                    avg /= 100.0;
                    fprintf(stderr, "      line %d: avg_mid=%.0f  [0]=%d [455]=%d [909]=%d\n",
                            ln, avg, row[0], row[fmt.output_line_len/2], row[fmt.output_line_len-1]);
                }
            }
        }

        // K6: Chroma decode — use full 2*spf for raw buffer access
        int field_phase_id = 0;
        chroma_decode(raw.data(), linelocs.data(), scratch.data(),
                      tbc_chroma.data(), is_first_field, spf, buf_len,
                      &field_phase_id, fmt, chroma_state, chroma_demod);

        // K7: Dropout detection + concealment
        auto dropouts = dropout_detect(envelope.data(), linelocs.data(),
                                       tbc_luma.data(), tbc_chroma.data(),
                                       buf_len, fmt);

        // Write output
        writer.write_luma_field(tbc_luma.data());
        writer.write_chroma_field(tbc_chroma.data());

        writer.set_first_field(is_first_field == 1);
        if (field_phase_id > 0) {
            writer.set_field_phase_id(field_phase_id);
        }
        for (auto& d : dropouts) {
            writer.add_dropout(d.line, d.startx, d.endx);
        }
        writer.finish_field();

        fields_done++;

        // VSYNC-locked advancement with drift correction.
        //
        // Problem: with fixed file_offset += spf, actual fields are ~900
        // samples shorter than nominal spf. The detected VSYNC (linelocs[0])
        // drifts ~900 samples/field until it falls off the buffer (~150 fields),
        // causing periodic tearing.
        //
        // Fix: target a stable VSYNC position in the buffer (spf/4). Apply a
        // proportional correction: advance = spf + (ll0 - target). This
        // converges in ~1 iteration and self-corrects for any field length.
        //
        // Target spf/4 ensures linelocs[0] > 0 (line 0 is ~19 lines before
        // VSYNC, needing ~34K samples of lead-in) and the next VSYNC at
        // ~target + actual_field_len < spf stays within sync_pulses' scan range.
        double ll0 = linelocs[0];
        double target_ll0 = spf * 0.25;
        if (ll0 > (double)(-spf) && ll0 < (double)(spf * 2)) {
            double advance = spf + (ll0 - target_ll0);
            if (advance > spf * 0.5 && advance < spf * 1.5) {
                file_offset += (size_t)(advance + 0.5);
            } else {
                file_offset += spf;
            }
        } else {
            file_offset += spf;
        }

        if (fields_done % 100 == 0) {
            fprintf(stderr, "\rProcessed %d fields...", fields_done);
        }
    }

    fprintf(stderr, "\rProcessed %d fields total.\n", fields_done);
    return writer.finalize();
}

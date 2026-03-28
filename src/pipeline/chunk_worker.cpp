#include "pipeline/chunk_worker.h"
#include "pipeline/sync_pulses.h"
#include "pipeline/line_locs.h"
#include "pipeline/hsync_refine.h"
#include "pipeline/tbc_resample.h"
#include "pipeline/chroma_decode.h"
#include "pipeline/dropout_detect.h"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <mutex>

// Thread-local pread + convert (same logic as RawReader::read_at but standalone).
static size_t read_samples(int fd, double* dest, size_t offset, size_t num_samples,
                           InputFormat input_fmt, size_t file_size) {
    int bps = input_format_bytes_per_sample(input_fmt);
    size_t byte_offset = offset * bps;
    if (byte_offset >= file_size) return 0;

    size_t available = (file_size - byte_offset) / bps;
    size_t to_read = (num_samples < available) ? num_samples : available;
    size_t byte_count = to_read * bps;

    auto* buf = new uint8_t[byte_count];
    ssize_t n = pread(fd, buf, byte_count, byte_offset);
    if (n <= 0) {
        delete[] buf;
        return 0;
    }
    size_t samples_read = n / bps;

    // Convert (same as RawReader::convert)
    switch (input_fmt) {
        case InputFormat::U8: {
            auto* src = reinterpret_cast<const uint8_t*>(buf);
            for (size_t i = 0; i < samples_read; i++)
                dest[i] = (static_cast<double>(src[i]) - 128.0) * 256.0;
            break;
        }
        case InputFormat::S16: {
            auto* src = reinterpret_cast<const int16_t*>(buf);
            for (size_t i = 0; i < samples_read; i++)
                dest[i] = static_cast<double>(src[i]);
            break;
        }
        case InputFormat::U16: {
            auto* src = reinterpret_cast<const uint16_t*>(buf);
            for (size_t i = 0; i < samples_read; i++)
                dest[i] = static_cast<double>(src[i]) - 32768.0;
            break;
        }
    }

    delete[] buf;
    return samples_read;
}

ChunkResult process_chunk(
    int worker_id,
    int reader_fd,
    InputFormat input_fmt,
    size_t chunk_start,
    size_t chunk_end,
    size_t overlap_end,
    int provisional_field_offset,
    TBCPWriter& pwriter,
    const VideoFormat& fmt,
    std::mutex& fftw_mutex,
    std::atomic<int>& fields_done,
    int fft_tile_size)
{
    ChunkResult result;
    result.worker_id = worker_id;
    result.chunk_start = chunk_start;
    result.chunk_end = chunk_end;

    int spf = fmt.samples_per_field;
    int buf_len = 2 * spf;

    // Get file size for bounds checking
    off_t fsize = lseek(reader_fd, 0, SEEK_END);
    if (fsize < 0) {
        fprintf(stderr, "Worker %d: lseek failed\n", worker_id);
        return result;
    }
    size_t file_size = (size_t)fsize;

    // Init FFTW plans under mutex (plan creation is not thread-safe).
    // FFTW_MEASURE benchmarks algorithms on first run (~5-15s per size).
    // With wisdom loaded from disk, this is instant.
    FMDemodState fm_state;
    ChromaDemodState chroma_demod;
    {
        std::lock_guard<std::mutex> lock(fftw_mutex);
        auto plan_t0 = std::chrono::steady_clock::now();
        if (!fm_state.init(fmt, buf_len, fft_tile_size)) {
            fprintf(stderr, "Worker %d: FM demod init failed\n", worker_id);
            return result;
        }
        if (!chroma_demod.init(fmt)) {
            fprintf(stderr, "Worker %d: chroma demod init failed\n", worker_id);
            return result;
        }
        auto plan_t1 = std::chrono::steady_clock::now();
        double plan_sec = std::chrono::duration<double>(plan_t1 - plan_t0).count();
        if (plan_sec > 0.1) {
            fprintf(stderr, "Worker %d: FFTW planning took %.1fs\n", worker_id, plan_sec);
        }
    }

    ChromaState chroma_state;

    // Per-field buffers
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

    size_t file_offset = chunk_start;
    int fields_in_chunk = 0;

    // Per-stage timing accumulators (worker 0 only, to avoid overhead on all)
    double t_read = 0, t_fm = 0, t_sync = 0, t_lineloc = 0, t_hsync = 0;
    double t_tbc = 0, t_chroma = 0, t_dropout = 0, t_write = 0;
    bool do_timing = (worker_id == 0);
    auto tick = [&]() { return std::chrono::steady_clock::now(); };
    using dur = std::chrono::duration<double>;

    while (file_offset < overlap_end) {
        auto t0 = tick();

        size_t n = read_samples(reader_fd, raw.data(), file_offset, buf_len,
                                input_fmt, file_size);
        if (n == 0) break;
        if ((int)n < buf_len) {
            memset(raw.data() + n, 0, (buf_len - n) * sizeof(double));
        }

        auto t1 = tick();

        // K1: FM demodulation
        fm_demod(fm_state, raw.data(), demod.data(), demod05.data(),
                 envelope.data(), buf_len, fmt);

        auto t2 = tick();

        // K2: Sync pulse detection
        int num_pulses = sync_pulses(demod05.data(), pulse_starts, pulse_lengths,
                                     spf, fmt);

        auto t3 = tick();

        // K3: Line locations
        int is_first_field = -1;
        line_locs(pulse_starts, pulse_lengths, num_pulses,
                  linelocs.data(), &is_first_field, spf, fmt);

        auto t4 = tick();

        // K4: Hsync refinement
        hsync_refine(demod05.data(), linelocs.data(), buf_len, fmt);

        auto t5 = tick();

        // K5: TBC resample (luma)
        tbc_resample(demod.data(), linelocs.data(), tbc_luma.data(), buf_len, fmt);

        auto t6 = tick();

        // K6: Chroma decode
        int field_phase_id = 0;
        chroma_decode(raw.data(), linelocs.data(), scratch.data(),
                      tbc_chroma.data(), is_first_field, spf, buf_len,
                      &field_phase_id, fmt, chroma_state, chroma_demod);

        auto t7 = tick();

        // K7: Dropout detection
        auto dropouts = dropout_detect(envelope.data(), linelocs.data(),
                                       tbc_luma.data(), tbc_chroma.data(),
                                       buf_len, fmt);

        auto t8 = tick();

        if (do_timing) {
            t_read += dur(t1 - t0).count();
            t_fm += dur(t2 - t1).count();
            t_sync += dur(t3 - t2).count();
            t_lineloc += dur(t4 - t3).count();
            t_hsync += dur(t5 - t4).count();
            t_tbc += dur(t6 - t5).count();
            t_chroma += dur(t7 - t6).count();
            t_dropout += dur(t8 - t7).count();
        }

        // Compute absolute VSYNC position
        double ll0 = linelocs[0];
        double vsync_abs = (double)file_offset + ll0;

        // Record field result
        FieldResult fr;
        fr.is_first_field = is_first_field;
        fr.field_phase_id = field_phase_id;
        fr.lineloc0 = ll0;
        fr.file_offset = file_offset;
        fr.dropouts = std::move(dropouts);

        // Write TBC data at provisional position
        int out_idx = provisional_field_offset + fields_in_chunk;

        auto tw0 = tick();
        pwriter.write_luma_field(out_idx, tbc_luma.data());
        pwriter.write_chroma_field(out_idx, tbc_chroma.data());
        auto tw1 = tick();
        if (do_timing) t_write += dur(tw1 - tw0).count();

        result.fields.push_back(std::move(fr));
        fields_in_chunk++;
        fields_done.fetch_add(1, std::memory_order_relaxed);

        // VSYNC-locked advancement (same proportional correction as sequential)
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
    }

    result.ok = true;
    fprintf(stderr, "Worker %d: decoded %d fields [samples %zu-%zu]\n",
            worker_id, fields_in_chunk, chunk_start, chunk_end);

    if (do_timing && fields_in_chunk > 0) {
        double total = t_read + t_fm + t_sync + t_lineloc + t_hsync +
                       t_tbc + t_chroma + t_dropout + t_write;
        fprintf(stderr, "  [PROFILE] Worker 0 per-stage totals (%.1fs, %d fields, %.1f ms/field):\n",
                total, fields_in_chunk, 1000.0 * total / fields_in_chunk);
        fprintf(stderr, "    read=%.1fs (%.0f%%)  fm_demod=%.1fs (%.0f%%)  sync=%.1fs (%.0f%%)\n",
                t_read, 100*t_read/total, t_fm, 100*t_fm/total, t_sync, 100*t_sync/total);
        fprintf(stderr, "    lineloc=%.1fs (%.0f%%)  hsync=%.1fs (%.0f%%)  tbc=%.1fs (%.0f%%)\n",
                t_lineloc, 100*t_lineloc/total, t_hsync, 100*t_hsync/total, t_tbc, 100*t_tbc/total);
        fprintf(stderr, "    chroma=%.1fs (%.0f%%)  dropout=%.1fs (%.0f%%)  write=%.1fs (%.0f%%)\n",
                t_chroma, 100*t_chroma/total, t_dropout, 100*t_dropout/total, t_write, 100*t_write/total);
    }

    return result;
}

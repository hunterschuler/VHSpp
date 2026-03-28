#include "pipeline/async_orchestrator.h"
#include "pipeline/chunk_worker.h"
#include "util/cache_info.h"
#include <cstdio>
#include <cmath>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>

// Stitch two adjacent chunk results at their boundary.
// Returns the index in chunk_b.fields where the "new" fields start
// (i.e., how many fields from chunk_b overlap with chunk_a's tail).
//
// Matching: find a VSYNC in chunk_b whose absolute position is within
// ±tolerance of a VSYNC in chunk_a's tail. The first match determines
// the stitch point.
static int find_stitch_point(const ChunkResult& chunk_a, const ChunkResult& chunk_b,
                             int spf) {
    if (chunk_a.fields.empty() || chunk_b.fields.empty()) return 0;

    double tolerance = 500.0;  // samples

    // Check last few fields of chunk_a against first few of chunk_b
    int a_check = std::min((int)chunk_a.fields.size(), 10);
    int b_check = std::min((int)chunk_b.fields.size(), 10);

    // Debug: show VSYNC positions at boundary
    fprintf(stderr, "  Stitch %d/%d: a_tail(%d fields from end) vs b_head(%d fields)\n",
            chunk_a.worker_id, chunk_b.worker_id, a_check, b_check);
    fprintf(stderr, "    chunk_a end=%zu  chunk_b start=%zu\n",
            chunk_a.chunk_end, chunk_b.chunk_start);

    for (int ai = (int)chunk_a.fields.size() - std::min(a_check, 4);
         ai < (int)chunk_a.fields.size(); ai++) {
        if (ai < 0) continue;
        double a_vs = (double)chunk_a.fields[ai].file_offset + chunk_a.fields[ai].lineloc0;
        fprintf(stderr, "    a[%d]: offset=%zu ll0=%.1f vsync_abs=%.1f\n",
                ai, chunk_a.fields[ai].file_offset, chunk_a.fields[ai].lineloc0, a_vs);
    }
    for (int bi = 0; bi < std::min(b_check, 4); bi++) {
        double b_vs = (double)chunk_b.fields[bi].file_offset + chunk_b.fields[bi].lineloc0;
        fprintf(stderr, "    b[%d]: offset=%zu ll0=%.1f vsync_abs=%.1f\n",
                bi, chunk_b.fields[bi].file_offset, chunk_b.fields[bi].lineloc0, b_vs);
    }

    // Find the first b field that matches an a field, then count how many
    // consecutive b fields also have matches (the full overlap extent).
    int first_bi = -1;
    int first_ai = -1;

    for (int bi = 0; bi < b_check && first_bi < 0; bi++) {
        double b_vsync = (double)chunk_b.fields[bi].file_offset +
                         chunk_b.fields[bi].lineloc0;

        for (int ai = (int)chunk_a.fields.size() - a_check;
             ai < (int)chunk_a.fields.size(); ai++) {
            if (ai < 0) continue;
            double a_vsync = (double)chunk_a.fields[ai].file_offset +
                             chunk_a.fields[ai].lineloc0;

            if (std::abs(b_vsync - a_vsync) < tolerance) {
                first_bi = bi;
                first_ai = ai;
                break;
            }
        }
    }

    if (first_bi < 0) {
        fprintf(stderr, "  Warning: no VSYNC match at chunk boundary %d/%d\n",
                chunk_a.worker_id, chunk_b.worker_id);
        return 0;
    }

    // Count consecutive matching fields after the first match
    int overlap_count = 1;
    for (int k = 1; k < b_check - first_bi; k++) {
        int bi = first_bi + k;
        int ai = first_ai + k;
        if (ai >= (int)chunk_a.fields.size()) break;

        double b_vsync = (double)chunk_b.fields[bi].file_offset +
                         chunk_b.fields[bi].lineloc0;
        double a_vsync = (double)chunk_a.fields[ai].file_offset +
                         chunk_a.fields[ai].lineloc0;

        if (std::abs(b_vsync - a_vsync) < tolerance) {
            overlap_count++;
        } else {
            break;
        }
    }

    int trim = first_bi + overlap_count;
    fprintf(stderr, "    => Match: a[%d..%d] vs b[%d..%d], trim %d fields\n",
            first_ai, first_ai + overlap_count - 1,
            first_bi, first_bi + overlap_count - 1, trim);
    return trim;
}

bool AsyncOrchestrator::run(RawReader& reader, const std::string& output_base,
                            const VideoFormat& fmt, int num_threads, bool overwrite) {
    if (!reader.is_seekable()) {
        fprintf(stderr, "Async orchestrator requires seekable input (file mode)\n");
        return false;
    }

    size_t total_samples = reader.total_samples();
    int spf = fmt.samples_per_field;
    int overlap = 2 * spf;  // 1 field of overlap on each side

    if (num_threads <= 0) {
        num_threads = std::max(1, (int)std::thread::hardware_concurrency() - 1);
    }

    // For very short files, don't use more threads than fields
    int est_fields = (int)(total_samples / spf);
    if (est_fields < num_threads * 4) {
        num_threads = std::max(1, est_fields / 4);
    }

    fprintf(stderr, "Async orchestrator: %zu samples, ~%d fields, %d workers\n",
            total_samples, est_fields, num_threads);

    // Compute chunk boundaries.
    // Each chunk owns [chunk_start, chunk_end) and reads up to chunk_end + overlap.
    // Overlap ensures the worker can decode fields near the boundary.
    struct ChunkBounds {
        size_t start;
        size_t end;         // exclusive, owned region
        size_t overlap_end; // exclusive, including overlap
        int provisional_field_offset;
    };

    size_t chunk_size = total_samples / num_threads;
    // Round to spf boundary for cleaner alignment
    chunk_size = ((chunk_size + spf - 1) / spf) * spf;

    std::vector<ChunkBounds> chunks(num_threads);
    int provisional_offset = 0;

    for (int i = 0; i < num_threads; i++) {
        chunks[i].start = (i == 0) ? 0 : chunks[i-1].end;
        chunks[i].end = (i == num_threads - 1) ? total_samples
                                                : chunks[i].start + chunk_size;
        chunks[i].overlap_end = std::min(chunks[i].end + overlap, total_samples);
        chunks[i].provisional_field_offset = provisional_offset;

        // Estimate fields in this chunk (including overlap)
        int est = (int)((chunks[i].overlap_end - chunks[i].start) / spf) + 2;
        provisional_offset += est;
    }

    // Pre-allocate output (generous estimate, will truncate after)
    int max_fields = provisional_offset + num_threads * 4;
    TBCPWriter pwriter;
    if (!pwriter.open(output_base, fmt, max_fields, overwrite)) {
        return false;
    }

    // Get the reader's fd for pread (workers need raw fd)
    // We'll re-open the file to get an fd we can share.
    // Actually, RawReader uses pread which is thread-safe, but we need
    // the raw fd. For now, open the file path again.
    // TODO: expose fd from RawReader or pass path directly.

    // Extract input file path from reader — we need to re-derive it.
    // For now, require the caller to pass it or we use /proc/self/fd.
    // Actually, let's just dup() it via /proc/self/fd hack, or better:
    // read the fd from RawReader. Since RawReader doesn't expose fd,
    // we'll use the /proc approach.

    // Simpler: we know the reader has a seekable fd. We can get it via
    // the file path that main.cpp opened. But we don't have it here.
    // Let's add a getter... or just pass it.
    //
    // WORKAROUND: We'll re-open via /proc/self/fd/<N>. This works on Linux.
    // The fd is the first member of RawReader.
    int reader_fd = *reinterpret_cast<const int*>(&reader);  // fd is first member

    // Verify it's valid
    if (fcntl(reader_fd, F_GETFL) < 0) {
        fprintf(stderr, "Failed to access reader fd\n");
        return false;
    }

    InputFormat input_fmt = reader.format();
    std::mutex fftw_mutex;
    std::atomic<int> fields_done{0};
    std::atomic<bool> workers_finished{false};

    // Tiling disabled — full-field FFT only (tiling produced visual artifacts,
    // see LOGBOOK.md "Tiled FM demod: attempted, investigated, abandoned")
    int fft_tile_size = 0;

    // Launch worker threads
    std::vector<ChunkResult> results(num_threads);
    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&, i]() {
            results[i] = process_chunk(
                i, reader_fd, input_fmt,
                chunks[i].start, chunks[i].end, chunks[i].overlap_end,
                chunks[i].provisional_field_offset,
                pwriter, fmt, fftw_mutex, fields_done, fft_tile_size);
        });
    }

    // Progress thread — prints status every 500ms
    std::thread progress_thread([&]() {
        auto start = std::chrono::steady_clock::now();
        while (!workers_finished.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            int done = fields_done.load(std::memory_order_relaxed);
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - start).count();
            double fps = (elapsed > 0.1) ? done / elapsed : 0;
            fprintf(stderr, "\r  %d fields  %.1f fps  %.1fs", done, fps, elapsed);
        }
        fprintf(stderr, "\n");
    });

    // Wait for all workers
    for (auto& t : threads) {
        t.join();
    }
    workers_finished.store(true, std::memory_order_relaxed);
    progress_thread.join();

    int total_decoded = fields_done.load();
    fprintf(stderr, "All workers done: %d fields decoded\n", total_decoded);

    // Check for failures
    for (int i = 0; i < num_threads; i++) {
        if (!results[i].ok) {
            fprintf(stderr, "Worker %d failed\n", i);
            return false;
        }
    }

    // Stitch: determine which fields from each chunk to keep.
    // Chunk 0 keeps all its fields. For chunks 1..N, trim overlapping
    // fields from the beginning.
    struct KeptRange {
        int chunk_idx;
        int start_field;  // first field index to keep (within chunk's fields)
        int end_field;    // exclusive
    };

    std::vector<KeptRange> kept;
    kept.push_back({0, 0, (int)results[0].fields.size()});

    for (int i = 1; i < num_threads; i++) {
        int trim = find_stitch_point(results[i-1], results[i], spf);
        int count = (int)results[i].fields.size() - trim;
        if (count > 0) {
            kept.push_back({i, trim, (int)results[i].fields.size()});
        }
    }

    // Count total output fields
    int total_output_fields = 0;
    for (auto& k : kept) {
        total_output_fields += k.end_field - k.start_field;
    }

    fprintf(stderr, "Stitching: %d total output fields from %d chunks\n",
            total_output_fields, num_threads);

    // Compact: remap fields from provisional positions to final contiguous positions.
    // For fields that are already in the right place, no copy needed.
    // For others, read from provisional position and write to final position.
    //
    // Build final metadata and remap TBC data.
    std::vector<TBCPWriter::FieldMeta> final_meta;
    final_meta.reserve(total_output_fields);

    int final_idx = 0;
    int field_byte_size = pwriter.field_byte_size();
    std::vector<uint16_t> copy_buf(fmt.output_field_lines * fmt.output_line_len);

    for (auto& k : kept) {
        auto& cr = results[k.chunk_idx];
        int prov_base = chunks[k.chunk_idx].provisional_field_offset;

        for (int fi = k.start_field; fi < k.end_field; fi++) {
            int prov_idx = prov_base + fi;
            auto& fr = cr.fields[fi];

            // If provisional index != final index, we need to copy the data
            if (prov_idx != final_idx) {
                // Read from provisional, write to final
                // For luma: pread from prov_idx offset, pwrite to final_idx
                // We can use the pwriter's fd... but it doesn't expose them.
                // Instead, read via pread on the output file.
                //
                // Simpler approach: just re-read/re-write via pwriter's internal fds.
                // We'll need a copy method. For now, use the OS-level approach.
                //
                // Actually, the simplest thing: pread from the output file at
                // prov_idx * field_bytes, pwrite at final_idx * field_bytes.
                // But we need the fds... Let's add a copy_field method to TBCPWriter.
                //
                // For MVP: mark as needing compaction and handle below.
            }

            // Build metadata
            TBCPWriter::FieldMeta fm;
            fm.is_first_field = (fr.is_first_field == 1);
            fm.field_phase_id = fr.field_phase_id;
            for (size_t di = 0; di < fr.dropouts.size(); di++) {
                TBCPWriter::FieldMeta::Dropout d;
                d.line = fr.dropouts[di].line;
                d.start = fr.dropouts[di].startx;
                d.end = fr.dropouts[di].endx;
                fm.dropouts.push_back(d);
            }
            final_meta.push_back(std::move(fm));

            final_idx++;
        }
    }

    // Compaction pass: move field data to final contiguous positions.
    // We need to do this carefully to avoid overwriting data we still need.
    // Since workers wrote at provisional offsets (which are >= final offsets
    // due to overlap estimates), we can safely compact left-to-right.
    //
    // We need raw access to the output fds for this. Add a compact method.
    // For now, re-open the output files for reading.
    int luma_rd = ::open((output_base + ".tbc").c_str(), O_RDONLY);
    int chroma_rd = ::open((output_base + "_chroma.tbc").c_str(), O_RDONLY);

    if (luma_rd < 0 || chroma_rd < 0) {
        fprintf(stderr, "Failed to open output files for compaction\n");
        if (luma_rd >= 0) ::close(luma_rd);
        if (chroma_rd >= 0) ::close(chroma_rd);
        return false;
    }

    // Re-open for writing (we need both read and write on the same file)
    int luma_wr = ::open((output_base + ".tbc").c_str(), O_WRONLY);
    int chroma_wr = ::open((output_base + "_chroma.tbc").c_str(), O_WRONLY);

    std::vector<uint8_t> field_buf(field_byte_size);
    final_idx = 0;

    for (auto& k : kept) {
        int prov_base = chunks[k.chunk_idx].provisional_field_offset;

        for (int fi = k.start_field; fi < k.end_field; fi++) {
            int prov_idx = prov_base + fi;

            if (prov_idx != final_idx) {
                off_t src_off = (off_t)field_byte_size * prov_idx;
                off_t dst_off = (off_t)field_byte_size * final_idx;

                // Copy luma
                if (pread(luma_rd, field_buf.data(), field_byte_size, src_off) < 0)
                    perror("pread luma compact");
                if (pwrite(luma_wr, field_buf.data(), field_byte_size, dst_off) < 0)
                    perror("pwrite luma compact");

                // Copy chroma
                if (pread(chroma_rd, field_buf.data(), field_byte_size, src_off) < 0)
                    perror("pread chroma compact");
                if (pwrite(chroma_wr, field_buf.data(), field_byte_size, dst_off) < 0)
                    perror("pwrite chroma compact");
            }

            final_idx++;
        }
    }

    ::close(luma_rd);
    ::close(chroma_rd);
    ::close(luma_wr);
    ::close(chroma_wr);

    // Set metadata and finalize
    pwriter.set_field_meta(std::move(final_meta));
    return pwriter.finalize();
}

#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <vector>
#include "format/video_format.h"
#include "pipeline/fm_demod.h"
#include "pipeline/chroma_decode.h"
#include "pipeline/dropout_detect.h"
#include "io/tbc_pwriter.h"

// Result of processing one field within a chunk.
struct FieldResult {
    int is_first_field = -1;
    int field_phase_id = 0;
    double lineloc0 = 0.0;             // VSYNC position (for stitching)
    size_t file_offset = 0;            // absolute sample offset this field was read from
    std::vector<Dropout> dropouts;
    // TBC data is written directly to pwriter, not stored here.
};

// Per-chunk output collected by the worker.
struct ChunkResult {
    int worker_id = -1;
    size_t chunk_start = 0;            // absolute sample offset of chunk start
    size_t chunk_end = 0;              // absolute sample offset of chunk end
    std::vector<FieldResult> fields;   // fields decoded, in order
    bool ok = false;
};

// Process one chunk of raw RF data. Runs the full pipeline (FM demod through
// dropout detection) on each field found in [chunk_start, chunk_end) with
// overlap region extending to chunk_end + overlap.
//
// Worker writes TBC data at provisional_field_offset + i for field i.
// The supervisor will compact/remap after stitching.
//
// reader_fd: file descriptor for pread() (from RawReader)
// pwriter: parallel writer (thread-safe pwrite per field)
// provisional_field_offset: output field index for this chunk's first field
// fftw_mutex: serialize FFTW plan creation (only used during init)
ChunkResult process_chunk(
    int worker_id,
    int reader_fd,
    InputFormat input_fmt,
    size_t chunk_start,       // first sample of this chunk's region
    size_t chunk_end,         // last sample (exclusive) of owned region
    size_t overlap_end,       // last sample (exclusive) including overlap
    int provisional_field_offset,
    TBCPWriter& pwriter,
    const VideoFormat& fmt,
    std::mutex& fftw_mutex,
    std::atomic<int>& fields_done,
    int fft_tile_size);       // 0 = full-field FFT, >0 = overlap-save tile size

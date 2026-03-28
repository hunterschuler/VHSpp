#pragma once
#include "format/video_format.h"
#include "io/raw_reader.h"
#include "io/tbc_pwriter.h"

// Async (file mode) parallel orchestrator.
//
// Divides the input file into N chunks with overlap regions, launches one
// worker thread per chunk, then stitches the results into a contiguous
// output TBC.
//
// Architecture:
//   1. Compute chunk boundaries with overlap (1 field of overlap per boundary)
//   2. Pre-allocate output files (estimated field count)
//   3. Launch workers — each runs the full pipeline on its chunk
//   4. Join all workers, stitch overlap regions (match VSYNCs)
//   5. Compact output (remove duplicate overlap fields, rewrite with pwrite)
//   6. Write JSON metadata and finalize
struct AsyncOrchestrator {
    bool run(RawReader& reader, const std::string& output_base,
             const VideoFormat& fmt, int num_threads, bool overwrite);
};

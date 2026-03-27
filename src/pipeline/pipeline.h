#pragma once
#include "format/video_format.h"
#include "io/raw_reader.h"
#include "io/tbc_writer.h"

// Top-level pipeline orchestrator.
//
// Currently a placeholder — will implement B+overlap supervisor/worker model.
// The design is orchestrator-agnostic at the stage level: all pipeline stages
// are pure single-field transforms. This orchestrator is the only component
// that knows about threading, chunks, and scheduling strategy.
//
// Future alternatives (same stage code, different orchestrator):
//   - Option C / work queue for live streaming
//   - Single-threaded sequential for debug/testing
struct Pipeline {
    bool run(RawReader& reader, TBCWriter& writer, const VideoFormat& fmt,
             int num_threads = 0);  // 0 = auto-detect core count
};

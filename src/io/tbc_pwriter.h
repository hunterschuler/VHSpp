#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>
#include "format/video_format.h"

// Parallel-safe TBC writer using pwrite() for concurrent field output.
//
// Workers write luma/chroma field data at known byte offsets without
// synchronization. Metadata (field_meta) is collected after all workers
// finish via set_field_meta(), then written once by the supervisor.
//
// File layout: each field is output_line_len * output_field_lines * sizeof(uint16_t)
// bytes. Field N starts at N * field_byte_size.
struct TBCPWriter {
    ~TBCPWriter();

    // Open output files and pre-allocate to total_fields size.
    bool open(const std::string& output_base, const VideoFormat& fmt,
              int total_fields, bool overwrite);
    void close();

    // Write one field at a specific field index. Thread-safe (uses pwrite).
    bool write_luma_field(int field_index, const uint16_t* data);
    bool write_chroma_field(int field_index, const uint16_t* data);

    // Per-field metadata, same as TBCWriter::FieldMeta.
    struct FieldMeta {
        bool is_first_field = true;
        int field_phase_id = 0;
        struct Dropout { int line; int start; int end; };
        std::vector<Dropout> dropouts;
    };

    // Set metadata for all fields at once (called by supervisor after stitching).
    void set_field_meta(std::vector<FieldMeta>&& meta);

    // Truncate files to actual_fields (if fewer than pre-allocated).
    void truncate(int actual_fields);

    // Write .tbc.json and finalize.
    bool finalize();

    int field_byte_size() const { return field_bytes; }

private:
    int luma_fd = -1;
    int chroma_fd = -1;
    std::string json_path;
    std::string luma_path;
    std::string chroma_path;
    VideoFormat fmt = VideoFormat(VideoSystem::NTSC, 28.0);

    int field_bytes = 0;      // bytes per field
    int total_fields = 0;     // pre-allocated count
    std::vector<FieldMeta> field_meta;

    bool write_json(int field_count);
};

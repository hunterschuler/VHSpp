#include "io/tbc_pwriter.h"
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

TBCPWriter::~TBCPWriter() {
    close();
}

bool TBCPWriter::open(const std::string& output_base, const VideoFormat& format,
                      int num_fields, bool overwrite) {
    fmt = format;
    total_fields = num_fields;
    field_bytes = fmt.output_line_len * fmt.output_field_lines * (int)sizeof(uint16_t);

    luma_path = output_base + ".tbc";
    chroma_path = output_base + "_chroma.tbc";
    json_path = output_base + ".tbc.json";

    if (!overwrite) {
        struct stat st;
        if (stat(luma_path.c_str(), &st) == 0) {
            fprintf(stderr, "Output exists: %s (use --overwrite)\n", luma_path.c_str());
            return false;
        }
    }

    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    mode_t mode = 0644;

    luma_fd = ::open(luma_path.c_str(), flags, mode);
    if (luma_fd < 0) {
        perror(luma_path.c_str());
        return false;
    }

    chroma_fd = ::open(chroma_path.c_str(), flags, mode);
    if (chroma_fd < 0) {
        perror(chroma_path.c_str());
        ::close(luma_fd);
        luma_fd = -1;
        return false;
    }

    // Pre-allocate files to avoid fragmentation and allow pwrite at any offset.
    off_t total_size = (off_t)field_bytes * total_fields;
    if (ftruncate(luma_fd, total_size) != 0) {
        perror("ftruncate luma");
        close();
        return false;
    }
    if (ftruncate(chroma_fd, total_size) != 0) {
        perror("ftruncate chroma");
        close();
        return false;
    }

    return true;
}

void TBCPWriter::close() {
    if (luma_fd >= 0) { ::close(luma_fd); luma_fd = -1; }
    if (chroma_fd >= 0) { ::close(chroma_fd); chroma_fd = -1; }
}

bool TBCPWriter::write_luma_field(int field_index, const uint16_t* data) {
    off_t offset = (off_t)field_bytes * field_index;
    ssize_t written = pwrite(luma_fd, data, field_bytes, offset);
    return written == field_bytes;
}

bool TBCPWriter::write_chroma_field(int field_index, const uint16_t* data) {
    off_t offset = (off_t)field_bytes * field_index;
    ssize_t written = pwrite(chroma_fd, data, field_bytes, offset);
    return written == field_bytes;
}

void TBCPWriter::set_field_meta(std::vector<FieldMeta>&& meta) {
    field_meta = std::move(meta);
}

void TBCPWriter::truncate(int actual_fields) {
    if (actual_fields < total_fields) {
        off_t size = (off_t)field_bytes * actual_fields;
        if (luma_fd >= 0 && ftruncate(luma_fd, size) != 0)
            perror("ftruncate luma");
        if (chroma_fd >= 0 && ftruncate(chroma_fd, size) != 0)
            perror("ftruncate chroma");
        total_fields = actual_fields;
    }
}

bool TBCPWriter::write_json(int field_count) {
    std::string tmp_path = json_path + ".tmp";
    FILE* fp = fopen(tmp_path.c_str(), "w");
    if (!fp) {
        perror(tmp_path.c_str());
        return false;
    }

    double black16b = (0.0 - fmt.vsync_ire) * fmt.output_scale + fmt.output_zero;
    double white16b = (100.0 - fmt.vsync_ire) * fmt.output_scale + fmt.output_zero;

    int burst_start = (int)(fmt.burst_start_us * 1e-6 * fmt.output_rate + 0.5);
    int burst_end   = (int)(fmt.burst_end_us * 1e-6 * fmt.output_rate + 0.5);

    // [NTSC-SPECIFIC]
    int active_start = 134;
    int active_end   = 894;

    fprintf(fp, "{\n");
    fprintf(fp, "  \"videoParameters\": {\n");
    fprintf(fp, "    \"system\": \"NTSC\",\n");
    fprintf(fp, "    \"isSubcarrierLocked\": false,\n");
    fprintf(fp, "    \"isSourcePal\": false,\n");
    fprintf(fp, "    \"numberOfSequentialFields\": %d,\n", field_count);
    fprintf(fp, "    \"black16bIre\": %.1f,\n", black16b);
    fprintf(fp, "    \"white16bIre\": %.1f,\n", white16b);
    fprintf(fp, "    \"sampleRate\": %.0f,\n", fmt.output_rate);
    fprintf(fp, "    \"fieldWidth\": %d,\n", fmt.output_line_len);
    fprintf(fp, "    \"fieldHeight\": %d,\n", fmt.output_field_lines);
    fprintf(fp, "    \"colourBurstStart\": %d,\n", burst_start);
    fprintf(fp, "    \"colourBurstEnd\": %d,\n", burst_end);
    fprintf(fp, "    \"activeVideoStart\": %d,\n", active_start);
    fprintf(fp, "    \"activeVideoEnd\": %d,\n", active_end);
    fprintf(fp, "    \"tapeFormat\": \"VHS\",\n");
    fprintf(fp, "    \"isMapped\": false\n");
    fprintf(fp, "  },\n");

    fprintf(fp, "  \"fields\": [\n");
    for (int i = 0; i < field_count; i++) {
        const auto& f = field_meta[i];
        fprintf(fp, "    {\n");
        fprintf(fp, "      \"isFirstField\": %s,\n", f.is_first_field ? "true" : "false");
        fprintf(fp, "      \"seqNo\": %d,\n", i + 1);

        if (f.field_phase_id > 0) {
            fprintf(fp, "      \"fieldPhaseID\": %d,\n", f.field_phase_id);
        }

        fprintf(fp, "      \"dropOuts\": {\n");
        fprintf(fp, "        \"fieldLine\": [");
        for (size_t j = 0; j < f.dropouts.size(); j++) {
            if (j > 0) fprintf(fp, ", ");
            fprintf(fp, "%d", f.dropouts[j].line);
        }
        fprintf(fp, "],\n");
        fprintf(fp, "        \"startx\": [");
        for (size_t j = 0; j < f.dropouts.size(); j++) {
            if (j > 0) fprintf(fp, ", ");
            fprintf(fp, "%d", f.dropouts[j].start);
        }
        fprintf(fp, "],\n");
        fprintf(fp, "        \"endx\": [");
        for (size_t j = 0; j < f.dropouts.size(); j++) {
            if (j > 0) fprintf(fp, ", ");
            fprintf(fp, "%d", f.dropouts[j].end);
        }
        fprintf(fp, "]\n");
        fprintf(fp, "      }\n");

        fprintf(fp, "    }%s\n", (i < field_count - 1) ? "," : "");
    }
    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");

    fclose(fp);

    if (rename(tmp_path.c_str(), json_path.c_str()) != 0) {
        perror("rename .tbc.json");
        return false;
    }

    return true;
}

bool TBCPWriter::finalize() {
    int actual = (int)field_meta.size();
    truncate(actual);

    if (!write_json(actual)) return false;

    fprintf(stderr, "\nWrote %s (%d fields)\n", json_path.c_str(), actual);
    close();
    return true;
}

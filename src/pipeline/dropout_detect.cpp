#include "pipeline/dropout_detect.h"
#include <cmath>
#include <algorithm>
#include <numeric>

// ---------------------------------------------------------------------------
// dropout_detect — Port of cuVHS dropout detection kernels to single-field CPU.
//
// Detects RF-level dropouts from the envelope signal, maps them to TBC
// line/column coordinates, and conceals by copying from adjacent lines.
// Pure transform: (envelope, linelocs, fmt) → dropout list + concealment.
//
// [NTSC-SPECIFIC] active_line_start, output_field_lines, output_line_len
// are all NTSC VHS SP values from VideoFormat.
// ---------------------------------------------------------------------------

// Constants matching Python vhs-decode defaults
static constexpr double DOD_THRESHOLD_P = 0.18;   // 18% of field mean envelope
static constexpr double DOD_HYSTERESIS  = 1.25;
static constexpr int    DOD_MERGE_DIST  = 30;      // merge nearby dropouts (RF samples)
static constexpr int    DOD_MIN_LENGTH  = 10;      // discard short dropouts
static constexpr int    ENV_BLOCK_SIZE  = 16;

// ---------------------------------------------------------------------------
// Helper: binary search linelocs to find which TBC line contains rf_pos.
// Returns line index (0-based relative to active_line_start), or -1 if not found.
// ---------------------------------------------------------------------------
static int find_tbc_line(const double* linelocs,
                         int active_line_start,
                         int n_lines,
                         double rf_pos)
{
    // linelocs[active_line_start .. active_line_start + n_lines] define line boundaries
    int lo = 0;
    int hi = n_lines - 1;

    // rf_pos must fall within [linelocs[active_line_start], linelocs[active_line_start + n_lines])
    if (rf_pos < linelocs[active_line_start] ||
        rf_pos >= linelocs[active_line_start + n_lines]) {
        return -1;
    }

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int ll_idx = mid + active_line_start;
        double line_begin = linelocs[ll_idx];
        double line_end   = linelocs[ll_idx + 1];

        if (rf_pos < line_begin) {
            hi = mid - 1;
        } else if (rf_pos >= line_end) {
            lo = mid + 1;
        } else {
            return mid;  // rf_pos is within this line
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Internal: RF-domain dropout span
// ---------------------------------------------------------------------------
struct RFDropout {
    int start;  // RF sample index
    int end;    // RF sample index (exclusive)
};

std::vector<Dropout> dropout_detect(
    const double* envelope,
    const double* linelocs,
    uint16_t* tbc_luma,
    uint16_t* tbc_chroma,
    int samples_per_field,
    const VideoFormat& fmt)
{
    std::vector<Dropout> result;

    if (samples_per_field <= 0) return result;

    const int out_lines = fmt.output_field_lines;  // [NTSC-SPECIFIC] 263
    const int out_cols  = fmt.output_line_len;      // [NTSC-SPECIFIC] 910
    const int line_start = fmt.active_line_start;   // [NTSC-SPECIFIC] 10
    const int lpf = fmt.lines_per_frame;            // [NTSC-SPECIFIC] 525

    // -----------------------------------------------------------------------
    // Step 1: Compute mean envelope for this field
    // -----------------------------------------------------------------------
    double env_sum = 0.0;
    for (int i = 0; i < samples_per_field; i++) {
        env_sum += envelope[i];
    }
    double mean_env = env_sum / samples_per_field;

    // -----------------------------------------------------------------------
    // Step 2: Threshold with hysteresis
    // -----------------------------------------------------------------------
    double down_thresh = mean_env * DOD_THRESHOLD_P;
    double up_thresh   = down_thresh * DOD_HYSTERESIS;

    // -----------------------------------------------------------------------
    // Step 3: Determine scan range from linelocs
    // Scan the RF samples that correspond to the active video area.
    // -----------------------------------------------------------------------
    int scan_start = 0;
    int scan_end   = samples_per_field;

    if (line_start < lpf) {
        scan_start = std::max(0, static_cast<int>(std::floor(linelocs[line_start])));
    }
    int last_active = line_start + out_lines;
    if (last_active < lpf) {
        scan_end = std::min(samples_per_field,
                            static_cast<int>(std::ceil(linelocs[last_active])));
    }

    // -----------------------------------------------------------------------
    // Step 4: Scan envelope in blocks, detect dropout spans
    // -----------------------------------------------------------------------
    std::vector<RFDropout> rf_dropouts;
    bool in_dropout = false;
    int dropout_start = 0;

    int num_blocks = (scan_end - scan_start + ENV_BLOCK_SIZE - 1) / ENV_BLOCK_SIZE;

    for (int blk = 0; blk < num_blocks; blk++) {
        int b_start = scan_start + blk * ENV_BLOCK_SIZE;
        int b_end   = std::min(b_start + ENV_BLOCK_SIZE, scan_end);

        // Block mean envelope
        double blk_sum = 0.0;
        for (int i = b_start; i < b_end; i++) {
            blk_sum += envelope[i];
        }
        double blk_mean = blk_sum / (b_end - b_start);

        if (!in_dropout && blk_mean < down_thresh) {
            // Start dropout
            in_dropout = true;
            dropout_start = b_start;
        } else if (in_dropout && blk_mean > up_thresh) {
            // End dropout
            in_dropout = false;
            rf_dropouts.push_back({dropout_start, b_start});
        }
    }

    // Close any dropout still open at end of scan
    if (in_dropout) {
        rf_dropouts.push_back({dropout_start, scan_end});
    }

    // -----------------------------------------------------------------------
    // Step 4b: Merge nearby dropouts
    // -----------------------------------------------------------------------
    if (rf_dropouts.size() > 1) {
        std::vector<RFDropout> merged;
        merged.push_back(rf_dropouts[0]);
        for (size_t i = 1; i < rf_dropouts.size(); i++) {
            RFDropout& prev = merged.back();
            if (rf_dropouts[i].start - prev.end <= DOD_MERGE_DIST) {
                prev.end = rf_dropouts[i].end;
            } else {
                merged.push_back(rf_dropouts[i]);
            }
        }
        rf_dropouts = std::move(merged);
    }

    // -----------------------------------------------------------------------
    // Step 5: Filter out short dropouts
    // -----------------------------------------------------------------------
    rf_dropouts.erase(
        std::remove_if(rf_dropouts.begin(), rf_dropouts.end(),
                        [](const RFDropout& d) { return (d.end - d.start) < DOD_MIN_LENGTH; }),
        rf_dropouts.end());

    // -----------------------------------------------------------------------
    // Step 6: Map RF positions to TBC line/column, split multi-line dropouts
    // -----------------------------------------------------------------------
    // Guard: need enough linelocs entries
    int max_ll = line_start + out_lines;
    if (max_ll >= lpf) {
        max_ll = lpf - 1;
    }

    for (const auto& rfd : rf_dropouts) {
        if (result.size() >= MAX_DROPOUTS_PER_FIELD) break;

        int start_line = find_tbc_line(linelocs, line_start, out_lines, static_cast<double>(rfd.start));
        int end_line   = find_tbc_line(linelocs, line_start, out_lines, static_cast<double>(rfd.end - 1));

        if (start_line < 0 && end_line < 0) continue;
        if (start_line < 0) start_line = 0;
        if (end_line < 0) end_line = out_lines - 1;

        for (int tbc_line = start_line; tbc_line <= end_line; tbc_line++) {
            if (result.size() >= MAX_DROPOUTS_PER_FIELD) break;

            int ll_idx = tbc_line + line_start;
            if (ll_idx + 1 >= lpf) continue;

            double line_begin = linelocs[ll_idx];
            double line_end   = linelocs[ll_idx + 1];
            double line_len_rf = line_end - line_begin;

            if (line_len_rf <= 0.0) continue;

            // Clip dropout to this line's RF range
            double d_start_rf = std::max(static_cast<double>(rfd.start), line_begin);
            double d_end_rf   = std::min(static_cast<double>(rfd.end), line_end);

            // Convert RF offset to output column
            int startx = static_cast<int>((d_start_rf - line_begin) / line_len_rf * out_cols);
            int endx   = static_cast<int>((d_end_rf - line_begin) / line_len_rf * out_cols);

            startx = std::clamp(startx, 0, out_cols - 1);
            endx   = std::clamp(endx, 0, out_cols - 1);

            if (endx <= startx) continue;

            result.push_back({tbc_line, startx, endx});
        }
    }

    // -----------------------------------------------------------------------
    // Step 7: Conceal — copy from adjacent line (prefer above, fallback below)
    // -----------------------------------------------------------------------
    for (const auto& d : result) {
        int src_line = -1;
        if (d.line > 0) {
            src_line = d.line - 1;  // prefer line above
        } else if (d.line + 1 < out_lines) {
            src_line = d.line + 1;  // fallback: line below
        }

        if (src_line < 0) continue;

        uint16_t* dst_luma = tbc_luma + d.line * out_cols;
        uint16_t* src_luma = tbc_luma + src_line * out_cols;

        for (int x = d.startx; x < d.endx; x++) {
            dst_luma[x] = src_luma[x];
        }

        // Conceal chroma if buffer is provided (non-null)
        if (tbc_chroma) {
            uint16_t* dst_chr = tbc_chroma + d.line * out_cols;
            uint16_t* src_chr = tbc_chroma + src_line * out_cols;

            for (int x = d.startx; x < d.endx; x++) {
                dst_chr[x] = src_chr[x];
            }
        }
    }

    return result;
}

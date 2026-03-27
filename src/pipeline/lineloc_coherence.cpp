#include "pipeline/lineloc_coherence.h"
#include <cmath>

// Temporal coherence pass for one field's line locations.
// Ported from cuVHS k_lineloc_coherence + k_extract_ref_positions.

static constexpr int COHERENCE_WINDOW = 10;  // look at ±10 fields

void lineloc_coherence(double* all_linelocs,
                       int field_index,
                       int num_fields_in_chunk,
                       const VideoFormat& fmt)
{
    int lines_per_frame = fmt.lines_per_frame;

    // Reference lines: average lines [active_line_start+10 .. +14]
    // (early active video, past vblank — robust reference)
    int ref_line_start = fmt.active_line_start + 10;
    int ref_line_count = 5;

    // Extract reference position for this field
    auto extract_ref = [&](int field) -> double {
        double sum = 0.0;
        int count = 0;
        for (int l = ref_line_start; l < ref_line_start + ref_line_count && l < lines_per_frame; l++) {
            sum += all_linelocs[field * lines_per_frame + l];
            count++;
        }
        return (count > 0) ? (sum / count) : 0.0;
    };

    double my_ref = extract_ref(field_index);

    // Gather neighbor reference positions (±COHERENCE_WINDOW fields, skip self)
    double neighbors[COHERENCE_WINDOW * 2];
    int n = 0;
    for (int d = -COHERENCE_WINDOW; d <= COHERENCE_WINDOW; d++) {
        int f = field_index + d;
        if (f >= 0 && f < num_fields_in_chunk && d != 0) {
            neighbors[n++] = extract_ref(f);
        }
    }

    if (n < 4) return;  // not enough context

    // Partial selection sort to find median
    int mid = n / 2;
    for (int i = 0; i <= mid; i++) {
        for (int j = i + 1; j < n; j++) {
            if (neighbors[j] < neighbors[i]) {
                double tmp = neighbors[i];
                neighbors[i] = neighbors[j];
                neighbors[j] = tmp;
            }
        }
    }
    double median = neighbors[mid];

    // Threshold: 10% of a line length
    double threshold = fmt.samples_per_line * 0.1;

    double offset = my_ref - median;

    if (fabs(offset) > threshold) {
        // Shift ALL this field's linelocs by the correction offset
        for (int line = 0; line < lines_per_frame; line++) {
            all_linelocs[field_index * lines_per_frame + line] -= offset;
        }
    }
}

#pragma once
#include <cstdint>
#include <vector>
#include "format/video_format.h"

#define MAX_DROPOUTS_PER_FIELD 512

struct Dropout {
    int line;
    int startx;
    int endx;
};

// Detect RF dropouts and map to TBC positions for one field.
// envelope: RF envelope magnitude (samples_per_field)
// linelocs: line positions [lines_per_frame]
// tbc_luma, tbc_chroma: TBC output buffers for concealment (modified in-place)
// Returns vector of dropout entries for metadata.
std::vector<Dropout> dropout_detect(
    const double* envelope,
    const double* linelocs,
    uint16_t* tbc_luma,
    uint16_t* tbc_chroma,
    int samples_per_field,
    const VideoFormat& fmt);

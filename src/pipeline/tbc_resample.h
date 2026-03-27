#pragma once
#include <cstdint>
#include "format/video_format.h"

// Resample one field from capture timebase to 4*fsc output timebase.
// demod: FM-demodulated video signal in Hz (at capture rate)
// linelocs: line start positions [lines_per_frame] (from line_locs/hsync_refine)
// tbc_luma: output uint16 buffer [output_field_lines × output_line_len]
// total_demod_samples: bounds for demod array
void tbc_resample(const double* demod,
                  const double* linelocs,
                  uint16_t* tbc_luma,
                  int total_demod_samples,
                  const VideoFormat& fmt);

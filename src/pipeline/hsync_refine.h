#pragma once
#include "format/video_format.h"

// Refine line locations via hsync zero-crossing detection for one field.
//
// Two-pass zero-crossing detection on demod05 (sync signal in Hz domain):
//   Pass 1: Find initial crossing at pulse threshold (-20 IRE)
//   Pass 2: Measure sync/porch levels, refine at (sync+porch)/2 midpoint
//
// Prefers right (trailing/rising) edge over left (leading/falling) edge,
// as the trailing edge is less susceptible to FM demod overshoot.
//
// Matches Python refine_linelocs_hsync() from vhsdecode/sync.pyx.
void hsync_refine(const double* demod05,
                  double* linelocs,           // [lines_per_frame] in/out
                  int total_demod_samples,    // bounds for this field's demod data
                  const VideoFormat& fmt);

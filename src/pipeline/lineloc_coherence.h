#pragma once
#include "format/video_format.h"

// Temporal coherence pass for line locations.
//
// Compares this field's reference position to median of neighbors.
// Fixes fields where the VSYNC state machine locks onto the wrong pulse
// (e.g., near tape edits), producing a consistent shift across all lines.
//
// all_linelocs: pointer to linelocs for ALL fields in chunk (contiguous,
//               each field is lines_per_frame doubles)
// field_index: which field within all_linelocs we're checking
// num_fields_in_chunk: total fields available for context
//
// In VHSpp's B+overlap architecture, the worker's chunk includes overlap
// fields that provide temporal context. This function doesn't know about
// chunks or overlap — it just sees a contiguous array and an index.
void lineloc_coherence(double* all_linelocs,
                       int field_index,
                       int num_fields_in_chunk,
                       const VideoFormat& fmt);

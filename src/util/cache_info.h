#pragma once

struct CacheInfo {
    int l2_per_core_bytes;   // per-core L2 cache in bytes (0 if unknown)
    int l3_total_bytes;      // shared L3 cache in bytes (0 if unknown)
};

// Query CPU cache sizes from sysfs. Falls back to conservative defaults.
CacheInfo query_cache_info();

// Compute optimal FFT tile size for fm_demod overlap-save.
// Considers cache budget per worker, number of buffers, and FFTW-friendly sizes.
// Returns 0 if tiling is unnecessary (field fits in cache).
int compute_fft_tile_size(int num_workers, int total_field_samples);

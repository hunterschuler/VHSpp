#include "util/cache_info.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <dirent.h>

// Parse a size string like "256K", "1024K", "30720K", "32M"
static int parse_cache_size(const char* s) {
    char* end = nullptr;
    long val = strtol(s, &end, 10);
    if (end && (*end == 'K' || *end == 'k')) return (int)(val * 1024);
    if (end && (*end == 'M' || *end == 'm')) return (int)(val * 1024 * 1024);
    return (int)val;
}

static int read_int_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char buf[64];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);
    return atoi(buf);
}

static int read_size_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char buf[64];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);
    // Trim newline
    char* nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return parse_cache_size(buf);
}

CacheInfo query_cache_info() {
    CacheInfo info = {0, 0};

    // Scan /sys/devices/system/cpu/cpu0/cache/index*
    const char* base = "/sys/devices/system/cpu/cpu0/cache";
    DIR* dir = opendir(base);
    if (!dir) {
        // Fallback: conservative defaults
        info.l2_per_core_bytes = 256 * 1024;   // 256 KB
        info.l3_total_bytes    = 8 * 1024 * 1024; // 8 MB
        fprintf(stderr, "  Cache: using defaults (L2=256K, L3=8M)\n");
        return info;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "index", 5) != 0) continue;

        char path[256];

        snprintf(path, sizeof(path), "%s/%s/level", base, ent->d_name);
        int level = read_int_file(path);

        snprintf(path, sizeof(path), "%s/%s/type", base, ent->d_name);
        FILE* tf = fopen(path, "r");
        char type[32] = {};
        if (tf) { fgets(type, sizeof(type), tf); fclose(tf); }

        // Only care about data or unified caches
        bool is_data = (strstr(type, "Data") || strstr(type, "Unified"));
        if (!is_data) continue;

        snprintf(path, sizeof(path), "%s/%s/size", base, ent->d_name);
        int size = read_size_file(path);
        if (size <= 0) continue;

        if (level == 2 && size > info.l2_per_core_bytes) {
            info.l2_per_core_bytes = size;
        }
        if (level == 3 && size > info.l3_total_bytes) {
            info.l3_total_bytes = size;
        }
    }
    closedir(dir);

    // Fallbacks
    if (info.l2_per_core_bytes == 0) info.l2_per_core_bytes = 256 * 1024;
    if (info.l3_total_bytes == 0)    info.l3_total_bytes = 8 * 1024 * 1024;

    fprintf(stderr, "  Cache: L2=%dK/core  L3=%dK shared\n",
            info.l2_per_core_bytes / 1024, info.l3_total_bytes / 1024);

    return info;
}

// 7-smooth check for FFTW-friendly sizes
static bool is_7smooth(int n) {
    while (n % 2 == 0) n /= 2;
    while (n % 3 == 0) n /= 3;
    while (n % 5 == 0) n /= 5;
    while (n % 7 == 0) n /= 7;
    return n == 1;
}

static int prev_fft_size(int n) {
    while (n > 1 && !is_7smooth(n)) --n;
    return n;
}

int compute_fft_tile_size(int num_workers, int total_field_samples) {
    CacheInfo cache = query_cache_info();

    // FM demod working set per tile-sample:
    //   padded(8) + fft_half(~8) + analytic(16) + post_fft(~8) + angles(8)
    //   + rf_filter(~4) + fvideo(~8) + fvideo05(~8) = ~68 bytes/sample
    const int bytes_per_sample = 68;

    // Budget: the smaller of L2/core or L3/workers, at 80% utilization
    int l3_per_worker = cache.l3_total_bytes / std::max(num_workers, 1);
    int budget = std::min(cache.l2_per_core_bytes, l3_per_worker);
    budget = (int)(budget * 0.80);

    int tile = budget / bytes_per_sample;

    // Clamp to reasonable range
    tile = std::max(tile, 8192);      // minimum: 8K (frequency resolution)
    tile = std::min(tile, 131072);    // maximum: 128K (diminishing returns)

    // Must not exceed total_field_samples
    if (total_field_samples > 0)
        tile = std::min(tile, total_field_samples);

    // Round down to nearest 7-smooth for FFTW efficiency
    tile = prev_fft_size(tile);

    fprintf(stderr, "  Cache: tile=%d samples (budget=%dK, %d bytes/sample)\n",
            tile, budget / 1024, bytes_per_sample);

    return tile;
}

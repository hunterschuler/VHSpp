#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "vhsdecode_cpp/k1_context.h"

namespace vhsdecode_cpp {

struct DemodCacheReadResult {
    std::vector<double> input;
    std::vector<double> video;
    std::vector<double> video_05;
    std::vector<double> chroma;
    std::uint64_t startloc = 0;
};

// Literal-first single-thread port of the exercised lddecode DemodCache.read()
// behavior for the live decoder.
//
// !!! LOUD PERFORMANCE NOTE !!!
// This exists because the end-to-end driver was re-demodulating overlapping K1
// windows for every field. Neighboring fields share a lot of the same block
// data, and upstream DemodCache.read() avoids paying that cost repeatedly.
class DemodCache {
public:
    DemodCache(const std::filesystem::path& raw_path,
               const vhsdecode::cppport::K1BuildConfig& cfg,
               std::shared_ptr<const vhsdecode::cppport::K1Context> ctx,
               std::size_t blockcut,
               std::size_t blockcut_end,
               std::size_t cache_size = 256);

    std::optional<DemodCacheReadResult> read(std::uint64_t begin,
                                             std::uint64_t length,
                                             bool force_redo = false,
                                             bool include_input = false);

    void flush_demod();

    std::size_t blocksize() const { return blocksize_; }
    std::uint64_t file_size() const { return file_size_; }

private:
    struct BlockEntry {
        std::vector<double> rawinput;
        std::vector<double> input;
        std::vector<double> video;
        std::vector<double> video_05;
        std::vector<double> chroma;
        bool have_demod = false;
    };

    bool load_raw_block(std::uint64_t blocknum);
    bool ensure_demod_block(std::uint64_t blocknum, bool force_redo);
    void ensure_input_block(BlockEntry& block);
    void prune_cache();
    static void lru_touch(std::vector<std::uint64_t>& lru, std::uint64_t key);

    std::ifstream in_;
    vhsdecode::cppport::K1BuildConfig cfg_{};
    std::shared_ptr<const vhsdecode::cppport::K1Context> ctx_{};
    std::size_t blockcut_ = 0;
    std::size_t blockcut_end_ = 0;
    std::size_t blocksize_ = 0;
    std::uint64_t file_size_ = 0;
    std::size_t cache_size_ = 256;
    std::unordered_map<std::uint64_t, BlockEntry> blocks_;
    std::vector<std::uint64_t> lru_;
    std::size_t transient_keep_blocks_ = 0;
};

}  // namespace vhsdecode_cpp

#include "vhsdecode_cpp/demod_cache.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace vhsdecode_cpp {
using namespace vhsdecode::cppport;

namespace {

void append_vec(std::vector<double>& dst, const std::vector<double>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

}  // namespace

DemodCache::DemodCache(const std::filesystem::path& raw_path,
                       const K1BuildConfig& cfg,
                       K1Context ctx,
                       std::size_t blockcut,
                       std::size_t blockcut_end,
                       std::size_t cache_size)
    : in_(raw_path, std::ios::binary),
      cfg_(cfg),
      ctx_(std::move(ctx)),
      blockcut_(blockcut),
      blockcut_end_(blockcut_end),
      cache_size_(cache_size) {
    if (!in_.is_open()) throw std::runtime_error("failed to open raw file: " + raw_path.string());
    in_.seekg(0, std::ios::end);
    file_size_ = static_cast<std::uint64_t>(in_.tellg());
    in_.seekg(0, std::ios::beg);
    blocksize_ = cfg_.blocklen - (blockcut_ + blockcut_end_);
}

void DemodCache::lru_touch(std::vector<std::uint64_t>& lru, std::uint64_t key) {
    const auto it = std::find(lru.begin(), lru.end(), key);
    if (it != lru.end()) lru.erase(it);
    lru.insert(lru.begin(), key);
}

void DemodCache::prune_cache() {
    if (lru_.size() <= cache_size_) return;
    for (std::size_t i = cache_size_; i < lru_.size(); ++i) {
        blocks_.erase(lru_[i]);
    }
    lru_.resize(cache_size_);
}

bool DemodCache::load_raw_block(std::uint64_t blocknum) {
    if (blocks_.find(blocknum) != blocks_.end()) {
        lru_touch(lru_, blocknum);
        return true;
    }
    const std::uint64_t file_off = blocknum * blocksize_;
    if (file_off + cfg_.blocklen > file_size_) return false;

    std::vector<std::uint8_t> raw(cfg_.blocklen);
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(file_off), std::ios::beg);
    in_.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(cfg_.blocklen));
    if (in_.gcount() != static_cast<std::streamsize>(cfg_.blocklen)) return false;

    BlockEntry entry;
    entry.rawinput.resize(cfg_.blocklen);
    for (std::size_t i = 0; i < cfg_.blocklen; ++i) {
        entry.rawinput[i] = static_cast<double>(raw[i]);
    }
    blocks_.emplace(blocknum, std::move(entry));
    lru_touch(lru_, blocknum);
    prune_cache();
    return true;
}

bool DemodCache::ensure_demod_block(std::uint64_t blocknum, bool force_redo) {
    if (!load_raw_block(blocknum)) return false;
    auto& block = blocks_.at(blocknum);
    if (block.have_demod && !force_redo) {
        lru_touch(lru_, blocknum);
        return true;
    }

    DemodBlockInput input{};
    input.data = block.rawinput;
    // LOUD FAITHFUL-PORT NOTE:
    // Upstream VHS DemodCacheTape demodulates each cached block as an
    // independent rf.demodblock(...) call. It does NOT thread a mutable
    // VideoEQ/K1 state object from block to block through the cache. Carrying
    // ctx_.state across blocks here introduced a live-only luma DC/blanking
    // shift that did not show up in isolated kernel parity work.
    auto state = DemodBlockState{};
    const auto result = demodblock(input, ctx_.filters, ctx_.options, cfg_.freq_hz, &state);

    block.input.assign(result.data.begin() + static_cast<std::ptrdiff_t>(blockcut_),
                       result.data.end() - static_cast<std::ptrdiff_t>(blockcut_end_));
    block.video.assign(result.out_video.begin() + static_cast<std::ptrdiff_t>(blockcut_),
                       result.out_video.end() - static_cast<std::ptrdiff_t>(blockcut_end_));
    block.video_05.assign(result.out_video05.begin() + static_cast<std::ptrdiff_t>(blockcut_),
                          result.out_video05.end() - static_cast<std::ptrdiff_t>(blockcut_end_));
    block.chroma.assign(result.out_chroma.begin() + static_cast<std::ptrdiff_t>(blockcut_),
                        result.out_chroma.end() - static_cast<std::ptrdiff_t>(blockcut_end_));
    block.have_demod = true;
    lru_touch(lru_, blocknum);
    return true;
}

std::optional<DemodCacheReadResult> DemodCache::read(std::uint64_t begin,
                                                     std::uint64_t length,
                                                     bool force_redo) {
    if (begin >= file_size_) return std::nullopt;
    const std::uint64_t end = begin + length;
    const std::uint64_t first_block = begin / blocksize_;
    const std::uint64_t last_block = end / blocksize_;

    for (std::uint64_t b = first_block; b <= last_block; ++b) {
        if (!ensure_demod_block(b, force_redo)) return std::nullopt;
    }

    DemodCacheReadResult out;
    for (std::uint64_t b = first_block; b <= last_block; ++b) {
        const auto& block = blocks_.at(b);
        append_vec(out.input, block.input);
        append_vec(out.video, block.video);
        append_vec(out.video_05, block.video_05);
        append_vec(out.chroma, block.chroma);
    }
    out.startloc = first_block * blocksize_;
    prune_cache();
    return out;
}

void DemodCache::flush_demod() {
    for (auto& [_, block] : blocks_) {
        block.have_demod = false;
        block.input.clear();
        block.video.clear();
        block.video_05.clear();
        block.chroma.clear();
    }
}

}  // namespace vhsdecode_cpp

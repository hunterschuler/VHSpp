#pragma once

#include <cstddef>
#include <optional>

namespace vhsdecode_cpp {

struct HardwareInfo {
    std::optional<std::size_t> total_ram_bytes;
    std::optional<std::size_t> l3_cache_bytes;
    std::optional<int> logical_cpus;
};

HardwareInfo detect_hardware_info();

}  // namespace vhsdecode_cpp

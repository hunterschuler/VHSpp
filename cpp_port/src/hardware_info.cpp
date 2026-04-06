#include "vhsdecode_cpp/hardware_info.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <sys/sysinfo.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace vhsdecode_cpp {

namespace {

std::optional<std::size_t> parse_size_string(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::size_t value = 0;
    std::size_t i = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        value = (value * 10U) + static_cast<std::size_t>(text[i] - '0');
        ++i;
    }
    std::size_t scale = 1;
    if (i < text.size()) {
        switch (text[i]) {
        case 'K':
        case 'k':
            scale = 1024ULL;
            break;
        case 'M':
        case 'm':
            scale = 1024ULL * 1024ULL;
            break;
        case 'G':
        case 'g':
            scale = 1024ULL * 1024ULL * 1024ULL;
            break;
        default:
            scale = 1;
            break;
        }
    }
    return value * scale;
}

}  // namespace

HardwareInfo detect_hardware_info() {
    HardwareInfo info;
    const auto hc = std::thread::hardware_concurrency();
    if (hc > 0U) info.logical_cpus = static_cast<int>(hc);

#if defined(__linux__)
    struct sysinfo si {};
    if (sysinfo(&si) == 0) {
        info.total_ram_bytes = static_cast<std::size_t>(si.totalram) * static_cast<std::size_t>(si.mem_unit);
    }

    namespace fs = std::filesystem;
    std::optional<std::size_t> best_l3;
    for (int idx = 0; idx < 16; ++idx) {
        const fs::path base = fs::path("/sys/devices/system/cpu/cpu0/cache") / ("index" + std::to_string(idx));
        std::ifstream level_in(base / "level");
        std::ifstream type_in(base / "type");
        std::ifstream size_in(base / "size");
        if (!level_in.is_open() || !type_in.is_open() || !size_in.is_open()) continue;
        int level = 0;
        std::string type;
        std::string size_text;
        level_in >> level;
        type_in >> type;
        size_in >> size_text;
        if (level != 3 || type != "Unified") continue;
        const auto parsed = parse_size_string(size_text);
        if (!parsed.has_value()) continue;
        best_l3 = std::max(best_l3.value_or(0), *parsed);
    }
    info.l3_cache_bytes = best_l3;
#elif defined(__APPLE__)
    std::uint64_t memsize = 0;
    std::size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0) {
        info.total_ram_bytes = static_cast<std::size_t>(memsize);
    }
    std::uint64_t l3size = 0;
    len = sizeof(l3size);
    if (sysctlbyname("hw.l3cachesize", &l3size, &len, nullptr, 0) == 0 && l3size > 0) {
        info.l3_cache_bytes = static_cast<std::size_t>(l3size);
    }
    int logical = 0;
    len = sizeof(logical);
    if (sysctlbyname("hw.logicalcpu", &logical, &len, nullptr, 0) == 0 && logical > 0) {
        info.logical_cpus = logical;
    }
#elif defined(_WIN32)
    MEMORYSTATUSEX statex {};
    statex.dwLength = sizeof(statex);
    if (GlobalMemoryStatusEx(&statex)) {
        info.total_ram_bytes = static_cast<std::size_t>(statex.ullTotalPhys);
    }
    const DWORD cpu_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (cpu_count > 0) info.logical_cpus = static_cast<int>(cpu_count);

    DWORD needed = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &needed);
    if (needed > 0) {
        std::string buffer(needed, '\0');
        auto* ptr = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
        if (GetLogicalProcessorInformationEx(RelationCache, ptr, &needed)) {
            std::size_t offset = 0;
            std::size_t best_l3 = 0;
            while (offset < needed) {
                auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
                if (entry->Relationship == RelationCache &&
                    entry->Cache.Level == 3 &&
                    entry->Cache.Type == CacheUnified) {
                    best_l3 = std::max(best_l3, static_cast<std::size_t>(entry->Cache.CacheSize));
                }
                offset += entry->Size;
            }
            if (best_l3 > 0) info.l3_cache_bytes = best_l3;
        }
    }
#endif

    return info;
}

}  // namespace vhsdecode_cpp

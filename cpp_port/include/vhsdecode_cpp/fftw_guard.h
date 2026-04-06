#pragma once

#include <mutex>

namespace vhsdecode::cppport {

inline std::mutex& fftw_plan_mutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace vhsdecode::cppport

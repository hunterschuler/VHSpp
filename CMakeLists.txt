cmake_minimum_required(VERSION 3.16)
project(VHSpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FFTW3 (double precision)
find_library(FFTW3_LIBRARY fftw3 REQUIRED)
find_path(FFTW3_INCLUDE_DIR fftw3.h)

# pthreads
find_package(Threads REQUIRED)

add_executable(vhspp
    src/main.cpp
    src/format/video_format.cpp
    src/io/raw_reader.cpp
    src/io/tbc_writer.cpp
    src/pipeline/fm_demod.cpp
    src/pipeline/sync_pulses.cpp
    src/pipeline/line_locs.cpp
    src/pipeline/hsync_refine.cpp
    src/pipeline/lineloc_coherence.cpp
    src/pipeline/tbc_resample.cpp
    src/pipeline/chroma_decode.cpp
    src/pipeline/dropout_detect.cpp
    src/pipeline/pipeline.cpp
)

target_include_directories(vhspp PRIVATE src ${FFTW3_INCLUDE_DIR})
target_link_libraries(vhspp PRIVATE ${FFTW3_LIBRARY} Threads::Threads)

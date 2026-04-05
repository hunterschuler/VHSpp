add_library(vhsdecode_k1_port
    src/demod_cache.cpp
    src/demod_block.cpp
    src/k1_context.cpp
    src/chroma_afc.cpp
    src/chroma_phase.cpp
    src/chroma_process.cpp
    src/downscale_core.cpp
    src/metadata_core.cpp
    src/output_core.cpp
    src/readfield_control_core.cpp
    src/session_core.cpp
    src/session_loop_core.cpp
    src/sync_core.cpp
    src/resync_core.cpp
    src/resync_runtime.cpp
    src/resync_state.cpp
    src/vsync_serration.cpp
)

target_include_directories(vhsdecode_k1_port
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${FFTW_INCLUDE_DIR}
)

target_link_libraries(vhsdecode_k1_port
    PUBLIC
        ${FFTW_LIBRARY}
        soxr
)

add_executable(vhsdecode_k1_parity
    src/k1_parity_main.cpp
)

target_link_libraries(vhsdecode_k1_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k1_file_stream
    src/k1_file_stream_bench.cpp
)

target_link_libraries(vhsdecode_k1_file_stream
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k2_sync_parity
    src/k2_sync_parity_main.cpp
)

target_link_libraries(vhsdecode_k2_sync_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k2_pulse_parity
    src/k2_pulse_parity_main.cpp
)

target_link_libraries(vhsdecode_k2_pulse_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k2_vsync_parity
    src/k2_vsync_parity_main.cpp
)

target_link_libraries(vhsdecode_k2_vsync_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k2_refine_parity
    src/k2_refine_parity_main.cpp
)

target_link_libraries(vhsdecode_k2_refine_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k2_get_pulses_parity
    src/k2_get_pulses_parity_main.cpp
)

target_link_libraries(vhsdecode_k2_get_pulses_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k2_first_hsync_parity
    src/k2_first_hsync_parity_main.cpp
)

target_link_libraries(vhsdecode_k2_first_hsync_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k2_hsync_refine_parity
    src/k2_hsync_refine_parity_main.cpp
)

target_link_libraries(vhsdecode_k2_hsync_refine_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k3_phase_parity
    src/k3_phase_parity_main.cpp
)

target_link_libraries(vhsdecode_k3_phase_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k3_process_parity
    src/k3_process_parity_main.cpp
)

target_link_libraries(vhsdecode_k3_process_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k4_downscale_parity
    src/k4_downscale_parity_main.cpp
)

target_link_libraries(vhsdecode_k4_downscale_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k4_field_parity
    src/k4_field_parity_main.cpp
)

target_link_libraries(vhsdecode_k4_field_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k5_metadata_parity
    src/k5_metadata_parity_main.cpp
)

target_link_libraries(vhsdecode_k5_metadata_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k5_write_json_parity
    src/k5_write_json_parity_main.cpp
)

target_link_libraries(vhsdecode_k5_write_json_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k6_session_parity
    src/k6_session_parity_main.cpp
)

target_link_libraries(vhsdecode_k6_session_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k7_control_parity
    src/k7_control_parity_main.cpp
)

target_link_libraries(vhsdecode_k7_control_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k7_live_control_parity
    src/k7_live_control_parity_main.cpp
)

target_link_libraries(vhsdecode_k7_live_control_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_k8_session_loop_parity
    src/k8_session_loop_parity_main.cpp
)

target_link_libraries(vhsdecode_k8_session_loop_parity
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_ntsc_vhs_hotpath
    src/ntsc_vhs_hotpath_bench.cpp
)

target_link_libraries(vhsdecode_ntsc_vhs_hotpath
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_ntsc_vhs_decode
    src/ntsc_vhs_decode_main.cpp
)

target_link_libraries(vhsdecode_ntsc_vhs_decode
    PRIVATE
        vhsdecode_k1_port
)

add_executable(vhsdecode_decode
    src/ntsc_vhs_decode_main.cpp
)

target_link_libraries(vhsdecode_decode
    PRIVATE
        vhsdecode_k1_port
)

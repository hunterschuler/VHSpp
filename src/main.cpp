#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include "format/video_format.h"
#include "io/raw_reader.h"
#include "io/tbc_writer.h"
#include "pipeline/pipeline.h"
#include "pipeline/async_orchestrator.h"

static void usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options] <input.raw> <output_base>\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --format <u8|s16|u16>   Input sample format (default: s16)\n");
    fprintf(stderr, "  --rate <MHz>            Sample rate in MHz (default: 28.0)\n");
    // [NTSC-SPECIFIC] Only NTSC is supported. --system flag reserved for future use.
    fprintf(stderr, "  --overwrite             Overwrite existing output files\n");
    fprintf(stderr, "  --threads <N>           Worker threads (default: auto)\n");
    fprintf(stderr, "  -                       Read from stdin\n");
    fprintf(stderr, "\nOutput: <output_base>.tbc, <output_base>_chroma.tbc, <output_base>.tbc.json\n");
}

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_base;
    InputFormat input_fmt = InputFormat::S16;
    double sample_rate_mhz = 28.636;
    bool overwrite = false;
    bool use_stdin = false;
    int num_threads = 0;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "u8") == 0) input_fmt = InputFormat::U8;
            else if (strcmp(argv[i], "s16") == 0) input_fmt = InputFormat::S16;
            else if (strcmp(argv[i], "u16") == 0) input_fmt = InputFormat::U16;
            else {
                fprintf(stderr, "Unknown format: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            sample_rate_mhz = atof(argv[++i]);
        } else if (strcmp(argv[i], "--overwrite") == 0) {
            overwrite = true;
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-") == 0) {
            use_stdin = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else if (input_path.empty() && !use_stdin) {
            input_path = argv[i];
        } else if (output_base.empty()) {
            output_base = argv[i];
        }
    }

    if (output_base.empty()) {
        usage(argv[0]);
        return 1;
    }

    // Auto-detect input format from file extension if not explicitly set
    if (input_fmt == InputFormat::S16 && !input_path.empty()) {
        if (input_path.size() >= 3 && input_path.substr(input_path.size() - 3) == ".u8")
            input_fmt = InputFormat::U8;
        else if (input_path.size() >= 4 && input_path.substr(input_path.size() - 4) == ".u16")
            input_fmt = InputFormat::U16;
        else if (input_path.size() >= 4 && input_path.substr(input_path.size() - 4) == ".s16")
            input_fmt = InputFormat::S16;
    }

    // [NTSC-SPECIFIC] Only NTSC is supported
    VideoFormat fmt(VideoSystem::NTSC, sample_rate_mhz);
    fmt.print_info();

    // Open input
    RawReader reader;
    if (use_stdin) {
        if (!reader.open_stdin(input_fmt)) {
            fprintf(stderr, "Failed to open stdin\n");
            return 1;
        }
        fprintf(stderr, "Reading from stdin (%s)\n", input_format_name(input_fmt));
    } else {
        if (!reader.open(input_path, input_fmt)) {
            return 1;
        }
        size_t total = reader.total_samples();
        double duration = (double)total / fmt.sample_rate;
        fprintf(stderr, "Input: %s (%s, %.1f seconds, %zu samples)\n",
                input_path.c_str(), input_format_name(input_fmt), duration, total);
    }

    // Choose orchestration mode:
    //   - Async (parallel): seekable file + threads != 1
    //   - Sequential: stdin/pipe or --threads 1
    bool use_async = reader.is_seekable() && num_threads != 1;

    if (use_async) {
        if (num_threads <= 0) {
            num_threads = std::max(1, (int)std::thread::hardware_concurrency() - 1);
        }
        fprintf(stderr, "Mode: async parallel (%d threads)\n", num_threads);

        AsyncOrchestrator orchestrator;
        if (!orchestrator.run(reader, output_base, fmt, num_threads, overwrite)) {
            fprintf(stderr, "Async pipeline failed\n");
            return 1;
        }
    } else {
        fprintf(stderr, "Mode: sequential\n");

        TBCWriter writer;
        if (!writer.open(output_base, fmt, overwrite)) {
            return 1;
        }

        Pipeline pipeline;
        if (!pipeline.run(reader, writer, fmt, num_threads)) {
            fprintf(stderr, "Pipeline failed\n");
            return 1;
        }
    }

    return 0;
}

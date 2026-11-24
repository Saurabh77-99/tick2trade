// itch: command-line tool for ITCH 5.0 files.
//
//   itch gen      <out.itch> [--symbols N] [--messages N] [--seed N]
//   itch stats    <file.itch>
//   itch replay   <file.itch> [--book vector|map] [--symbol AAPL] [--depth N]
//                             [--latency] [--warm] [--check]
//   itch snapshot <file.itch> --symbol AAPL [--interval-ms N] [--depth N]
//                             [--from HH:MM] [--to HH:MM] [--out file.jsonl]

#include <cstdio>
#include <cstring>
#include <exception>

#include "args.hpp"

namespace hft::cli {
int cmd_gen(const Args&);
int cmd_stats(const Args&);
int cmd_replay(const Args&);
int cmd_snapshot(const Args&);
}  // namespace hft::cli

static void usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  itch gen      <out.itch> [--symbols 8] [--messages 2000000] [--seed 42]\n"
                 "  itch stats    <file.itch>\n"
                 "  itch replay   <file.itch> [--book vector|map] [--symbol AAPL] [--depth 10]\n"
                 "                            [--latency] [--warm] [--check]\n"
                 "  itch snapshot <file.itch> --symbol AAPL [--interval-ms 1000] [--depth 10]\n"
                 "                            [--from 09:30] [--to 16:00] [--out AAPL.jsonl]\n");
}

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    const hft::cli::Args args(argc, argv, 2);
    try {
        if (!std::strcmp(argv[1], "gen")) return hft::cli::cmd_gen(args);
        if (!std::strcmp(argv[1], "stats")) return hft::cli::cmd_stats(args);
        if (!std::strcmp(argv[1], "replay")) return hft::cli::cmd_replay(args);
        if (!std::strcmp(argv[1], "snapshot")) return hft::cli::cmd_snapshot(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}

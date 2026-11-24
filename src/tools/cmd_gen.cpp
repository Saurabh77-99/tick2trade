// itch gen: write a synthetic ITCH 5.0 day to a file.

#include <cstdio>
#include <stdexcept>

#include "args.hpp"
#include "hft/sim/synthetic_feed.hpp"

namespace hft::cli {

int cmd_gen(const Args& a) {
    if (a.positional().empty()) throw std::runtime_error("gen: missing output path");
    const std::string out = a.positional()[0];

    sim::FeedConfig cfg;
    cfg.symbols = static_cast<int>(a.num("symbols", 8));
    cfg.messages = a.num("messages", 2'000'000);
    cfg.seed = a.num("seed", 42);

    itch::Writer w;
    w.bytes().reserve(cfg.messages * 34);
    sim::SyntheticFeed feed(cfg);
    feed.generate(w);

    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + out);
    std::fwrite(w.bytes().data(), 1, w.bytes().size(), f);
    std::fclose(f);

    std::printf("wrote %s: %.1f MB, %d symbols, ~%llu messages, seed %llu\n", out.c_str(),
                double(w.bytes().size()) / 1e6, cfg.symbols, (unsigned long long)cfg.messages,
                (unsigned long long)cfg.seed);
    std::printf("symbols:");
    for (const auto& s : feed.symbols()) std::printf(" %s", s.c_str());
    std::printf("\n");
    return 0;
}

}  // namespace hft::cli

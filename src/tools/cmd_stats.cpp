// itch stats: count messages by type and measure raw parse speed
// (no book building), the upper bound for everything downstream.

#include <cstdio>
#include <stdexcept>

#include "args.hpp"
#include "hft/common/mapped_file.hpp"
#include "hft/common/timing.hpp"
#include "hft/itch/parser.hpp"

namespace hft::cli {

namespace {
// Touches every field so the compiler can't optimize the decoding away.
struct ChecksumHandler : itch::NullHandler {
    std::uint64_t sum = 0;
    void on_add(const itch::AddOrder& m) { sum += m.order_ref ^ m.price ^ m.shares; }
    void on_executed(const itch::OrderExecuted& m) { sum += m.order_ref ^ m.executed_shares; }
    void on_cancel(const itch::OrderCancel& m) { sum += m.order_ref ^ m.cancelled_shares; }
    void on_delete(const itch::OrderDelete& m) { sum += m.order_ref; }
    void on_replace(const itch::OrderReplace& m) { sum += m.new_ref ^ m.price; }
};

const char* type_name(char t) {
    switch (t) {
        case 'S': return "System Event";
        case 'R': return "Stock Directory";
        case 'H': return "Stock Trading Action";
        case 'Y': return "Reg SHO Restriction";
        case 'L': return "Market Participant Position";
        case 'V': return "MWCB Decline Level";
        case 'W': return "MWCB Status";
        case 'K': return "IPO Quoting Period";
        case 'J': return "LULD Auction Collar";
        case 'h': return "Operational Halt";
        case 'A': return "Add Order";
        case 'F': return "Add Order (MPID)";
        case 'E': return "Order Executed";
        case 'C': return "Order Executed w/ Price";
        case 'X': return "Order Cancel";
        case 'D': return "Order Delete";
        case 'U': return "Order Replace";
        case 'P': return "Trade (non-cross)";
        case 'Q': return "Cross Trade";
        case 'B': return "Broken Trade";
        case 'I': return "NOII";
        case 'N': return "RPII";
        case 'O': return "Direct Listing CRPD";
        default:  return "unknown";
    }
}
}  // namespace

int cmd_stats(const Args& a) {
    if (a.positional().empty()) throw std::runtime_error("stats: missing file");
    MappedFile file(a.positional()[0]);

    ChecksumHandler h;
    itch::ParseStats st;
    const std::uint64_t t0 = now_ns();
    itch::parse(file.data(), file.size(), h, st);
    const double secs = double(now_ns() - t0) / 1e9;

    std::printf("%-28s %14s %7s\n", "message type", "count", "share");
    for (int t = 0; t < 256; ++t) {
        if (!st.by_type[t]) continue;
        std::printf("  %c %-24s %14llu %6.2f%%\n", char(t), type_name(char(t)),
                    (unsigned long long)st.by_type[t], 100.0 * double(st.by_type[t]) / double(st.messages));
    }
    std::printf("total messages %llu, %.1f MB\n", (unsigned long long)st.messages, double(st.bytes) / 1e6);
    std::printf("parse only: %.3f s, %.1f M msg/s, %.1f ns/msg, %.2f GB/s\n", secs,
                double(st.messages) / secs / 1e6, secs * 1e9 / double(st.messages),
                double(st.bytes) / secs / 1e9);
    if (st.unknown_type || st.bad_length || st.truncated)
        std::printf("warnings: unknown types %llu, bad lengths %llu, truncated %s\n",
                    (unsigned long long)st.unknown_type, (unsigned long long)st.bad_length,
                    st.truncated ? "yes" : "no");
    std::printf("(checksum %llx)\n", (unsigned long long)h.sum);
    return 0;
}

}  // namespace hft::cli

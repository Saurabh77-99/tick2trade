#pragma once
// Zero-copy ITCH 5.0 BinaryFILE parser.
//
// The parser walks a byte buffer (normally an mmap'd file), decodes each
// message into a small struct on the stack, and calls the matching method on
// a Handler. The Handler is a template parameter, not a virtual interface:
// the compiler sees the concrete type, inlines the callbacks, and the whole
// "parse -> update book" path becomes one tight loop with no indirect calls.
//
// Handlers derive from NullHandler and override only what they need:
//
//   struct MyHandler : hft::itch::NullHandler {
//       void on_add(const AddOrder& m) { ... }
//   };
//
// The parser never allocates and never throws on the hot path. Malformed
// input (bad length, truncated tail) is counted in ParseStats.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "hft/common/endian.hpp"
#include "hft/itch/messages.hpp"

namespace hft::itch {

struct NullHandler {
    void on_system_event(const SystemEvent&) {}
    void on_stock_directory(const StockDirectory&) {}
    void on_trading_action(const StockTradingAction&) {}
    void on_add(const AddOrder&) {}
    void on_executed(const OrderExecuted&) {}
    void on_executed_with_price(const OrderExecutedWithPrice&) {}
    void on_cancel(const OrderCancel&) {}
    void on_delete(const OrderDelete&) {}
    void on_replace(const OrderReplace&) {}
    void on_trade(const Trade&) {}
    void on_cross_trade(const CrossTrade&) {}
    void on_broken_trade(const BrokenTrade&) {}
    // Any message type we don't decode (NOII, Reg SHO, MWCB, ...).
    void on_other(char /*type*/, const std::uint8_t* /*msg*/, std::size_t /*len*/) {}
};

struct ParseStats {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0;
    std::uint64_t by_type[256] = {};
    std::uint64_t unknown_type = 0;
    std::uint64_t bad_length = 0;   // length prefix disagrees with the spec
    bool truncated = false;         // buffer ended mid-message
};

namespace detail {

inline Header read_header(const std::uint8_t* p) noexcept {
    return Header{static_cast<char>(p[0]), load_be16(p + 1), load_be16(p + 3), load_be48(p + 5)};
}

inline void copy8(char (&dst)[8], const std::uint8_t* src) noexcept { std::memcpy(dst, src, 8); }

}  // namespace detail

// Decode and dispatch one message. `p` points at the type byte; `len` is the
// length from the prefix (already validated against message_size()).
template <typename Handler>
inline void dispatch(const std::uint8_t* p, std::size_t len, Handler& h) {
    using namespace detail;
    switch (static_cast<char>(p[0])) {
        case 'A':
        case 'F': {
            AddOrder m;
            m.h = read_header(p);
            m.order_ref = load_be64(p + 11);
            m.side = static_cast<Side>(p[19]);
            m.shares = load_be32(p + 20);
            copy8(m.stock, p + 24);
            m.price = load_be32(p + 32);
            if (p[0] == 'F') std::memcpy(m.mpid, p + 36, 4);
            else std::memset(m.mpid, ' ', 4);
            h.on_add(m);
            break;
        }
        case 'E': {
            OrderExecuted m;
            m.h = read_header(p);
            m.order_ref = load_be64(p + 11);
            m.executed_shares = load_be32(p + 19);
            m.match_number = load_be64(p + 23);
            h.on_executed(m);
            break;
        }
        case 'C': {
            OrderExecutedWithPrice m;
            m.h = read_header(p);
            m.order_ref = load_be64(p + 11);
            m.executed_shares = load_be32(p + 19);
            m.match_number = load_be64(p + 23);
            m.printable = static_cast<char>(p[31]);
            m.execution_price = load_be32(p + 32);
            h.on_executed_with_price(m);
            break;
        }
        case 'X': {
            OrderCancel m;
            m.h = read_header(p);
            m.order_ref = load_be64(p + 11);
            m.cancelled_shares = load_be32(p + 19);
            h.on_cancel(m);
            break;
        }
        case 'D': {
            OrderDelete m;
            m.h = read_header(p);
            m.order_ref = load_be64(p + 11);
            h.on_delete(m);
            break;
        }
        case 'U': {
            OrderReplace m;
            m.h = read_header(p);
            m.original_ref = load_be64(p + 11);
            m.new_ref = load_be64(p + 19);
            m.shares = load_be32(p + 27);
            m.price = load_be32(p + 31);
            h.on_replace(m);
            break;
        }
        case 'P': {
            Trade m;
            m.h = read_header(p);
            m.order_ref = load_be64(p + 11);
            m.side = static_cast<Side>(p[19]);
            m.shares = load_be32(p + 20);
            copy8(m.stock, p + 24);
            m.price = load_be32(p + 32);
            m.match_number = load_be64(p + 36);
            h.on_trade(m);
            break;
        }
        case 'Q': {
            CrossTrade m;
            m.h = read_header(p);
            m.shares = load_be64(p + 11);
            copy8(m.stock, p + 19);
            m.cross_price = load_be32(p + 27);
            m.match_number = load_be64(p + 31);
            m.cross_type = static_cast<char>(p[39]);
            h.on_cross_trade(m);
            break;
        }
        case 'B': {
            BrokenTrade m;
            m.h = read_header(p);
            m.match_number = load_be64(p + 11);
            h.on_broken_trade(m);
            break;
        }
        case 'S': {
            SystemEvent m;
            m.h = read_header(p);
            m.event_code = static_cast<char>(p[11]);
            h.on_system_event(m);
            break;
        }
        case 'R': {
            StockDirectory m;
            m.h = read_header(p);
            copy8(m.stock, p + 11);
            m.market_category = static_cast<char>(p[19]);
            m.financial_status = static_cast<char>(p[20]);
            m.round_lot_size = load_be32(p + 21);
            m.round_lots_only = static_cast<char>(p[25]);
            h.on_stock_directory(m);
            break;
        }
        case 'H': {
            StockTradingAction m;
            m.h = read_header(p);
            copy8(m.stock, p + 11);
            m.trading_state = static_cast<char>(p[19]);
            h.on_trading_action(m);
            break;
        }
        default:
            h.on_other(static_cast<char>(p[0]), p, len);
            break;
    }
}

// Parse every complete message in [data, data + size). Returns the number of
// bytes consumed (less than `size` only if the buffer ends mid-message).
template <typename Handler>
std::size_t parse(const std::uint8_t* data, std::size_t size, Handler& h, ParseStats& st) {
    std::size_t pos = 0;
    while (pos + 2 <= size) {
        const std::uint16_t len = load_be16(data + pos);
        if (pos + 2 + len > size) {
            st.truncated = true;
            break;
        }
        const std::uint8_t* msg = data + pos + 2;
        pos += 2 + std::size_t{len};
        if (len == 0) {
            ++st.bad_length;
            continue;
        }

        const char type = static_cast<char>(msg[0]);
        const std::uint16_t expected = message_size(type);
        ++st.messages;
        ++st.by_type[static_cast<std::uint8_t>(type)];
        if (expected == 0) {
            ++st.unknown_type;
            continue;
        }
        // Nasdaq may append fields in future revisions, so a longer message
        // is tolerated; a shorter one would make us read past its end.
        if (len < expected) {
            ++st.bad_length;
            continue;
        }
        dispatch(msg, len, h);
    }
    st.bytes += pos;
    return pos;
}

}  // namespace hft::itch

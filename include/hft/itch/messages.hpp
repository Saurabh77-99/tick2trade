#pragma once
// Nasdaq TotalView-ITCH 5.0 message definitions.
//
// Wire format (BinaryFILE, what Nasdaq's historical files use):
//   [2-byte big-endian length][message bytes] [2-byte length][message] ...
//
// Every message starts with the same 11-byte header:
//   offset 0  : message type        (1 byte, ASCII)
//   offset 1  : stock locate        (2)  per-day integer id for the symbol
//   offset 3  : tracking number     (2)  Nasdaq internal
//   offset 5  : timestamp           (6)  nanoseconds since midnight
//
// Prices are 4-byte unsigned integers with 4 implied decimals:
//   1234500 means $123.4500. We keep prices as integers end to end:
//   floating point can't represent $0.10 exactly, and integer compares are
//   faster. Convert to double only for display.
//
// The structs below are the *decoded* form (host byte order, natural
// alignment). The parser fills them from the wire; the encoder does the
// reverse. Field offsets come straight from the ITCH 5.0 specification.

#include <cstdint>
#include <string_view>

namespace hft::itch {

using Price = std::uint32_t;  // 4 implied decimals
inline constexpr double kPriceScale = 10000.0;

enum class Side : std::uint8_t { Buy = 'B', Sell = 'S' };

// Total message length on the wire (excluding the 2-byte length prefix),
// indexed by message type. 0 = unknown type.
constexpr std::uint16_t message_size(char type) noexcept {
    switch (type) {
        case 'S': return 12;  // System Event
        case 'R': return 39;  // Stock Directory
        case 'H': return 25;  // Stock Trading Action
        case 'Y': return 20;  // Reg SHO Restriction
        case 'L': return 26;  // Market Participant Position
        case 'V': return 35;  // MWCB Decline Level
        case 'W': return 12;  // MWCB Status
        case 'K': return 28;  // IPO Quoting Period Update
        case 'J': return 35;  // LULD Auction Collar
        case 'h': return 21;  // Operational Halt
        case 'A': return 36;  // Add Order (no MPID)
        case 'F': return 40;  // Add Order with MPID attribution
        case 'E': return 31;  // Order Executed
        case 'C': return 36;  // Order Executed With Price
        case 'X': return 23;  // Order Cancel (partial)
        case 'D': return 19;  // Order Delete
        case 'U': return 35;  // Order Replace
        case 'P': return 44;  // Trade (non-cross, hidden order)
        case 'Q': return 40;  // Cross Trade
        case 'B': return 19;  // Broken Trade
        case 'I': return 50;  // NOII (auction imbalance)
        case 'N': return 20;  // Retail Price Improvement Indicator
        case 'O': return 48;  // Direct Listing w/ Capital Raise
        default:  return 0;
    }
}

struct Header {
    char type;
    std::uint16_t locate;
    std::uint16_t tracking;
    std::uint64_t timestamp;  // ns since midnight
};

struct SystemEvent {           // 'S'
    Header h;
    char event_code;           // O,S,Q,M,E,C (see spec)
};

struct StockDirectory {        // 'R'
    Header h;
    char stock[8];             // space-padded, e.g. "AAPL    "
    char market_category;
    char financial_status;
    std::uint32_t round_lot_size;
    char round_lots_only;
};

struct StockTradingAction {    // 'H'
    Header h;
    char stock[8];
    char trading_state;        // H=halted, P=paused, Q=quotation only, T=trading
};

struct AddOrder {              // 'A' and 'F'
    Header h;
    std::uint64_t order_ref;
    Side side;
    std::uint32_t shares;
    char stock[8];
    Price price;
    char mpid[4];              // only meaningful for 'F'
};

struct OrderExecuted {         // 'E'
    Header h;
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
};

struct OrderExecutedWithPrice {  // 'C'
    Header h;
    std::uint64_t order_ref;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
    char printable;            // 'Y' = counts toward volume
    Price execution_price;
};

struct OrderCancel {           // 'X'
    Header h;
    std::uint64_t order_ref;
    std::uint32_t cancelled_shares;
};

struct OrderDelete {           // 'D'
    Header h;
    std::uint64_t order_ref;
};

struct OrderReplace {          // 'U'
    Header h;
    std::uint64_t original_ref;
    std::uint64_t new_ref;
    std::uint32_t shares;
    Price price;
};

struct Trade {                 // 'P'
    Header h;
    std::uint64_t order_ref;   // usually 0 for hidden orders
    Side side;
    std::uint32_t shares;
    char stock[8];
    Price price;
    std::uint64_t match_number;
};

struct CrossTrade {            // 'Q'
    Header h;
    std::uint64_t shares;
    char stock[8];
    Price cross_price;
    std::uint64_t match_number;
    char cross_type;
};

struct BrokenTrade {           // 'B'
    Header h;
    std::uint64_t match_number;
};

// "AAPL    " -> "AAPL"
inline std::string_view trim_stock(const char (&s)[8]) noexcept {
    std::size_t n = 8;
    while (n > 0 && s[n - 1] == ' ') --n;
    return {s, n};
}

}  // namespace hft::itch

#pragma once
// ITCH 5.0 encoder: the inverse of the parser.
//
// Used by the synthetic data generator and by tests (encode -> parse -> check
// the fields survived the round trip). Later, the exchange simulator from
// Phase 2 will use the same encoder to publish its market data feed.

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "hft/common/endian.hpp"
#include "hft/itch/messages.hpp"

namespace hft::itch {

class Writer {
public:
    const std::vector<std::uint8_t>& bytes() const noexcept { return buf_; }
    std::vector<std::uint8_t>& bytes() noexcept { return buf_; }
    void clear() noexcept { buf_.clear(); }

    void system_event(std::uint64_t ts, char code) {
        auto* p = begin('S', 0, ts);
        p[11] = static_cast<std::uint8_t>(code);
    }

    void stock_directory(std::uint16_t locate, std::uint64_t ts, std::string_view stock,
                         std::uint32_t round_lot = 100) {
        auto* p = begin('R', locate, ts);
        put_stock(p + 11, stock);
        p[19] = 'Q';  // market category: Nasdaq Global Select
        p[20] = 'N';  // financial status: normal
        store_be32(p + 21, round_lot);
        p[25] = 'N';  // round lots only: no
        p[26] = 'C';  // issue classification: common stock
        p[27] = 'Z'; p[28] = ' ';  // issue sub-type
        p[29] = 'P';  // authenticity: production
        p[30] = 'N';  // short sale threshold
        p[31] = 'N';  // IPO flag
        p[32] = '1';  // LULD reference price tier
        p[33] = 'N';  // ETP flag
        store_be32(p + 34, 0);  // ETP leverage factor
        p[38] = 'N';  // inverse indicator
    }

    void trading_action(std::uint16_t locate, std::uint64_t ts, std::string_view stock, char state) {
        auto* p = begin('H', locate, ts);
        put_stock(p + 11, stock);
        p[19] = static_cast<std::uint8_t>(state);
        p[20] = ' ';
        std::memcpy(p + 21, "    ", 4);  // reason
    }

    void add_order(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, Side side,
                   std::uint32_t shares, std::string_view stock, Price price,
                   std::string_view mpid = {}) {
        auto* p = begin(mpid.empty() ? 'A' : 'F', locate, ts);
        store_be64(p + 11, ref);
        p[19] = static_cast<std::uint8_t>(side);
        store_be32(p + 20, shares);
        put_stock(p + 24, stock);
        store_be32(p + 32, price);
        if (!mpid.empty()) {
            std::memset(p + 36, ' ', 4);
            std::memcpy(p + 36, mpid.data(), mpid.size() < 4 ? mpid.size() : 4);
        }
    }

    void order_executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                        std::uint32_t shares, std::uint64_t match) {
        auto* p = begin('E', locate, ts);
        store_be64(p + 11, ref);
        store_be32(p + 19, shares);
        store_be64(p + 23, match);
    }

    void order_executed_with_price(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref,
                                   std::uint32_t shares, std::uint64_t match, Price price,
                                   char printable = 'Y') {
        auto* p = begin('C', locate, ts);
        store_be64(p + 11, ref);
        store_be32(p + 19, shares);
        store_be64(p + 23, match);
        p[31] = static_cast<std::uint8_t>(printable);
        store_be32(p + 32, price);
    }

    void order_cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
        auto* p = begin('X', locate, ts);
        store_be64(p + 11, ref);
        store_be32(p + 19, shares);
    }

    void order_delete(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
        auto* p = begin('D', locate, ts);
        store_be64(p + 11, ref);
    }

    void order_replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t old_ref,
                       std::uint64_t new_ref, std::uint32_t shares, Price price) {
        auto* p = begin('U', locate, ts);
        store_be64(p + 11, old_ref);
        store_be64(p + 19, new_ref);
        store_be32(p + 27, shares);
        store_be32(p + 31, price);
    }

    void trade(std::uint16_t locate, std::uint64_t ts, Side side, std::uint32_t shares,
               std::string_view stock, Price price, std::uint64_t match) {
        auto* p = begin('P', locate, ts);
        store_be64(p + 11, 0);
        p[19] = static_cast<std::uint8_t>(side);
        store_be32(p + 20, shares);
        put_stock(p + 24, stock);
        store_be32(p + 32, price);
        store_be64(p + 36, match);
    }

    void cross_trade(std::uint16_t locate, std::uint64_t ts, std::uint64_t shares,
                     std::string_view stock, Price price, std::uint64_t match, char type) {
        auto* p = begin('Q', locate, ts);
        store_be64(p + 11, shares);
        put_stock(p + 19, stock);
        store_be32(p + 27, price);
        store_be64(p + 31, match);
        p[39] = static_cast<std::uint8_t>(type);
    }

    void broken_trade(std::uint16_t locate, std::uint64_t ts, std::uint64_t match) {
        auto* p = begin('B', locate, ts);
        store_be64(p + 11, match);
    }

private:
    // Appends the length prefix and common header; returns a pointer to the
    // message's first byte. The body is zero-filled.
    std::uint8_t* begin(char type, std::uint16_t locate, std::uint64_t ts) {
        const std::uint16_t len = message_size(type);
        const std::size_t off = buf_.size();
        buf_.resize(off + 2 + len, 0);
        std::uint8_t* base = buf_.data() + off;
        store_be16(base, len);
        std::uint8_t* p = base + 2;
        p[0] = static_cast<std::uint8_t>(type);
        store_be16(p + 1, locate);
        store_be16(p + 3, 0);
        store_be48(p + 5, ts);
        return p;
    }

    static void put_stock(std::uint8_t* dst, std::string_view s) {
        std::memset(dst, ' ', 8);
        std::memcpy(dst, s.data(), s.size() < 8 ? s.size() : 8);
    }

    std::vector<std::uint8_t> buf_;
};

}  // namespace hft::itch

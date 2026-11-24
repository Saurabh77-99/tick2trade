#pragma once
// Order entry: what traders send to the exchange, and what it sends back.
//
// Requests (trader -> exchange):
//   NewOrder     limit or market; time in force DAY / IOC / FOK; post-only
//   CancelOrder  remove a resting order
//   ModifyOrder  change price and/or quantity of a resting order
//
// Reports (exchange -> trader), delivered through a Listener:
//   Accepted  the order passed validation and has an exchange order id
//   Rejected  it didn't (reason says why); nothing changed
//   Fill      part or all of an order traded (one per side of each match)
//   Canceled  shares removed without trading (user request, IOC/FOK expiry,
//             self-trade prevention, market order with no liquidity)
//   Replaced  a modify took effect
//
// These mirror the message types of real order entry protocols (Nasdaq OUCH,
// FIX) without the wire format. Phase 3 puts a binary protocol in front.

#include <cstdint>

#include "hft/itch/messages.hpp"

namespace hft::exchange {

using itch::Price;
using itch::Side;

enum class TimeInForce : std::uint8_t {
    Day,  // rest any unfilled remainder in the book
    IOC,  // immediate-or-cancel: fill what you can now, cancel the rest
    FOK,  // fill-or-kill: fill everything now, or nothing at all
};

struct NewOrder {
    std::uint32_t trader;           // who is sending (owns the order)
    std::uint64_t client_order_id;  // trader's own id, echoed in reports
    std::uint16_t locate;           // symbol
    Side side;
    Price price;                    // ignored for market orders
    std::uint32_t qty;
    TimeInForce tif = TimeInForce::Day;
    bool market = false;            // take liquidity at any price; never rests
    bool post_only = false;         // must add liquidity; rejected if it would trade
};

struct CancelOrder {
    std::uint32_t trader;
    std::uint64_t order_id;         // exchange id from Accepted
};

struct ModifyOrder {
    std::uint32_t trader;
    std::uint64_t order_id;
    Price new_price;
    std::uint32_t new_qty;          // new open (leaves) quantity
};

enum class RejectReason : std::uint8_t {
    InvalidQty,
    InvalidPrice,       // zero, off-tick, or market + post-only
    UnknownSymbol,
    UnknownOrder,       // cancel/modify for an order that isn't live
    NotOwner,           // cancel/modify for someone else's order
    PostOnlyWouldCross,
};

enum class CancelReason : std::uint8_t {
    User,          // CancelOrder
    IocRemainder,  // IOC or market order: unfilled part
    FokUnfilled,   // FOK couldn't fill completely
    SelfTrade,     // resting order canceled by self-trade prevention
};

struct Accepted {
    std::uint64_t ts;
    std::uint32_t trader;
    std::uint64_t client_order_id;
    std::uint64_t order_id;
    std::uint16_t locate;
    Side side;
    Price price;
    std::uint32_t qty;
};

struct Rejected {
    std::uint64_t ts;
    std::uint32_t trader;
    std::uint64_t client_order_id;  // for new orders
    std::uint64_t order_id;         // for cancel / modify
    RejectReason reason;
};

struct Fill {
    std::uint64_t ts;
    std::uint32_t trader;
    std::uint64_t order_id;
    std::uint64_t client_order_id;
    std::uint16_t locate;
    Side side;
    Price price;
    std::uint32_t qty;
    std::uint32_t leaves;     // shares still open after this fill
    std::uint64_t match_id;   // same for both sides of a match
    bool maker;               // true: was resting (added liquidity)
};

struct Canceled {
    std::uint64_t ts;
    std::uint32_t trader;
    std::uint64_t order_id;
    std::uint64_t client_order_id;
    std::uint16_t locate;
    std::uint32_t qty;        // shares canceled
    CancelReason reason;
};

struct Replaced {
    std::uint64_t ts;
    std::uint32_t trader;
    std::uint64_t old_order_id;
    std::uint64_t new_order_id;  // equal to old_order_id when priority was kept
    std::uint64_t client_order_id;
    std::uint16_t locate;
    Price price;
    std::uint32_t qty;
};

// Listeners derive from this and override what they need (same pattern as
// itch::NullHandler: templates, no virtual calls).
struct NullListener {
    void on_accepted(const Accepted&) {}
    void on_rejected(const Rejected&) {}
    void on_fill(const Fill&) {}
    void on_canceled(const Canceled&) {}
    void on_replaced(const Replaced&) {}
};

inline const char* to_string(RejectReason r) {
    switch (r) {
        case RejectReason::InvalidQty: return "invalid qty";
        case RejectReason::InvalidPrice: return "invalid price";
        case RejectReason::UnknownSymbol: return "unknown symbol";
        case RejectReason::UnknownOrder: return "unknown order";
        case RejectReason::NotOwner: return "not owner";
        case RejectReason::PostOnlyWouldCross: return "post-only would cross";
    }
    return "?";
}

inline const char* to_string(CancelReason r) {
    switch (r) {
        case CancelReason::User: return "user";
        case CancelReason::IocRemainder: return "ioc remainder";
        case CancelReason::FokUnfilled: return "fok unfilled";
        case CancelReason::SelfTrade: return "self trade";
    }
    return "?";
}

}  // namespace hft::exchange

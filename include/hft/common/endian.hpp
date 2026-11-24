#pragma once
// Big-endian (network byte order) loads and stores.
//
// ITCH is big-endian; x86 and ARM are little-endian. We load with memcpy
// (the only portable, UB-free way to read unaligned bytes) and byte-swap with
// a compiler builtin. Optimizing compilers turn each of these into a single
// MOVBE or MOV+BSWAP instruction, so there's no cost to keeping it portable.

#include <cstdint>
#include <cstring>

namespace hft {

inline std::uint16_t load_be16(const std::uint8_t* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap16(v);
}

inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap32(v);
}

inline std::uint64_t load_be64(const std::uint8_t* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return __builtin_bswap64(v);
}

// ITCH timestamps are 6-byte (48-bit) nanoseconds since midnight.
inline std::uint64_t load_be48(const std::uint8_t* p) noexcept {
    return (std::uint64_t{load_be16(p)} << 32) | load_be32(p + 2);
}

inline void store_be16(std::uint8_t* p, std::uint16_t v) noexcept {
    v = __builtin_bswap16(v);
    std::memcpy(p, &v, sizeof v);
}

inline void store_be32(std::uint8_t* p, std::uint32_t v) noexcept {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, sizeof v);
}

inline void store_be64(std::uint8_t* p, std::uint64_t v) noexcept {
    v = __builtin_bswap64(v);
    std::memcpy(p, &v, sizeof v);
}

inline void store_be48(std::uint8_t* p, std::uint64_t v) noexcept {
    store_be16(p, static_cast<std::uint16_t>(v >> 32));
    store_be32(p + 2, static_cast<std::uint32_t>(v));
}

}  // namespace hft

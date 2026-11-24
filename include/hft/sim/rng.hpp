#pragma once
// Small, fast, reproducible random number generator (splitmix64).
//
// std::mt19937 is fine, but std::uniform_int_distribution and friends are
// implemented differently by libstdc++ and libc++, so the same seed gives
// different data on Linux vs macOS. Rolling our own distributions keeps
// generated files byte-identical everywhere, which matters when you compare
// benchmark numbers across machines.

#include <cmath>
#include <cstdint>

namespace hft::sim {

class Rng {
public:
    explicit Rng(std::uint64_t seed) : s_(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform integer in [0, n). Lemire's multiply-shift: no slow modulo.
    std::uint64_t below(std::uint64_t n) noexcept {
        __extension__ using u128 = unsigned __int128;  // GCC/Clang extension
        return static_cast<std::uint64_t>((static_cast<u128>(next()) * n) >> 64);
    }

    // Uniform double in [0, 1).
    double unit() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

    bool chance(double p) noexcept { return unit() < p; }

    // Number of failures before the first success, success probability p.
    std::uint32_t geometric(double p) noexcept {
        const double u = 1.0 - unit();  // (0, 1]
        return static_cast<std::uint32_t>(std::floor(std::log(u) / std::log(1.0 - p)));
    }

    // Exponential with the given mean.
    double exponential(double mean) noexcept { return -mean * std::log(1.0 - unit()); }

private:
    std::uint64_t s_;
};

}  // namespace hft::sim

#pragma once
// Cheap timestamps and a latency histogram.
//
// rdtsc reads the CPU's time-stamp counter in ~20 cycles, versus ~20-30 ns for
// clock_gettime. On modern x86 the TSC is "invariant": it ticks at a constant
// rate regardless of frequency scaling, so we can calibrate it once against
// steady_clock and convert ticks to nanoseconds.
//
// The histogram is log-linear (the idea behind HdrHistogram): each power of two
// is split into 2^kSubBits equal buckets, giving ~3% relative precision from
// 1 ns to hours in a fixed 8 KB table. Recording is a few instructions and
// never allocates, so it's safe to call on a hot path.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define HFT_HAS_RDTSC 1
#endif

namespace hft {

inline std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Raw tick counter: TSC on x86, steady_clock nanoseconds elsewhere.
inline std::uint64_t ticks() noexcept {
#ifdef HFT_HAS_RDTSC
    return __rdtsc();
#else
    return now_ns();
#endif
}

// Measures how many ticks make one nanosecond. Call once at startup.
inline double calibrate_ticks_per_ns() {
#ifdef HFT_HAS_RDTSC
    const std::uint64_t t0 = now_ns();
    const std::uint64_t c0 = __rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::uint64_t t1 = now_ns();
    const std::uint64_t c1 = __rdtsc();
    return static_cast<double>(c1 - c0) / static_cast<double>(t1 - t0);
#else
    return 1.0;
#endif
}

class LatencyHistogram {
public:
    static constexpr int kSubBits = 5;              // 32 sub-buckets per power of 2
    static constexpr int kSub = 1 << kSubBits;
    static constexpr int kMaxExp = 64 - kSubBits;   // covers the full uint64 range
    static constexpr int kBuckets = (kMaxExp + 1) * kSub;

    void record(std::uint64_t v) noexcept {
        ++counts_[index_of(v)];
        ++total_;
        max_ = std::max(max_, v);
        min_ = std::min(min_, v);
        sum_ += v;
    }

    std::uint64_t count() const noexcept { return total_; }
    std::uint64_t max() const noexcept { return max_; }
    std::uint64_t min() const noexcept { return total_ ? min_ : 0; }
    double mean() const noexcept { return total_ ? double(sum_) / double(total_) : 0.0; }

    // Value at quantile q in [0, 1]. Returns the upper edge of the bucket, so
    // the answer is never optimistic.
    std::uint64_t percentile(double q) const noexcept {
        if (total_ == 0) return 0;
        const auto target = static_cast<std::uint64_t>(q * double(total_ - 1)) + 1;
        std::uint64_t seen = 0;
        for (int i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= target) return std::min(upper_edge(i), max_);
        }
        return max_;
    }

    void merge(const LatencyHistogram& o) noexcept {
        for (int i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
        sum_ += o.sum_;
        max_ = std::max(max_, o.max_);
        min_ = std::min(min_, o.min_);
    }

    // Values are divided by `scale` when printed, so you can record raw TSC
    // ticks on the hot path and convert to ns only here.
    void print(const char* title, double scale = 1.0, const char* unit = "ns") const {
        std::printf("%s (n=%llu)\n", title, static_cast<unsigned long long>(total_));
        std::printf("  min    %10.1f %s\n", double(min()) / scale, unit);
        std::printf("  mean   %10.1f %s\n", mean() / scale, unit);
        const double qs[] = {0.50, 0.90, 0.99, 0.999, 0.9999};
        const char* names[] = {"p50", "p90", "p99", "p99.9", "p99.99"};
        for (int i = 0; i < 5; ++i)
            std::printf("  %-6s %10.1f %s\n", names[i], double(percentile(qs[i])) / scale, unit);
        std::printf("  max    %10.1f %s\n", double(max_) / scale, unit);
    }

    static int index_of(std::uint64_t v) noexcept {
        if (v < kSub) return static_cast<int>(v);  // exact for small values
        const int msb = 63 - __builtin_clzll(v);   // position of highest set bit
        const int shift = msb - kSubBits;
        const int sub = static_cast<int>((v >> shift) & (kSub - 1));
        return (shift + 1) * kSub + sub;
    }

    static std::uint64_t upper_edge(int idx) noexcept {
        if (idx < kSub) return static_cast<std::uint64_t>(idx);
        const int shift = idx / kSub - 1;
        const std::uint64_t sub = static_cast<std::uint64_t>(idx % kSub);
        return ((std::uint64_t{kSub} | sub) << shift) + ((std::uint64_t{1} << shift) - 1);
    }

private:
    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t total_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t max_ = 0;
    std::uint64_t min_ = ~std::uint64_t{0};
};

}  // namespace hft

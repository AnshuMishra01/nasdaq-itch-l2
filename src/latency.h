#pragma once

// M8: per-message latency measurement. A cycle-counter clock and a histogram that
// records one value per message without allocating.

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <x86intrin.h>
#include <cpuid.h>

namespace itch {

// ---- clock ---------------------------------------------------------------------------
// rdtsc counts at a fixed rate (the "invariant TSC"), independent of the core's current
// clock, and is not ordered with surrounding instructions. The fences stop the timed
// work leaking outside the pair:
//   begin: lfence waits for earlier instructions to finish, then read the counter
//   end:   rdtscp waits for the timed work to finish; lfence stops later work starting early
// On AMD, lfence is dispatch-serialising when the OS enables it (Windows and Linux do,
// as a Spectre mitigation).
inline std::uint64_t tsc_begin() {
    _mm_lfence();
    return __rdtsc();
}

inline std::uint64_t tsc_end() {
    unsigned aux;
    const std::uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

// Unfenced: plain rdtsc at both ends. Much cheaper, but the CPU may move the reads
// relative to the timed work, so single samples blur; the distribution stays usable.
inline std::uint64_t tsc_plain() { return __rdtsc(); }

// CPUID 0x80000007 EDX bit 8: the TSC runs at a constant rate in all power states.
inline bool invariant_tsc() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(0x80000007, &a, &b, &c, &d)) return false;
    return (d >> 8) & 1U;
}

// TSC ticks per nanosecond, by timing a busy-wait of `ms` against steady_clock.
inline double tsc_per_ns(int ms = 200) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t c0 = tsc_begin();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) {}
    const std::uint64_t c1 = tsc_end();
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return static_cast<double>(c1 - c0) / static_cast<double>(ns);
}

// ---- histogram -----------------------------------------------------------------------
// Log-linear buckets (the HDR-histogram idea): every power of two is split into 16
// equal sub-buckets. Pure log2 buckets are too coarse where the median sits (one bucket
// would be 64-127 ticks); pure linear buckets need a cap that throws away the tail.
// This covers 0 to 2^64 ticks in 976 buckets (7.8 KB) with at most 1/16 = 6.25%
// relative error, and values below 16 are exact.
class Histogram {
public:
    static constexpr unsigned kSubBits = 4;
    static constexpr std::uint64_t kSub = 1U << kSubBits;
    static constexpr std::size_t kBuckets = (64 - kSubBits + 1) * kSub;

    static std::size_t index(std::uint64_t v) {
        if (v < kSub) return static_cast<std::size_t>(v);
        const unsigned e = 63U - static_cast<unsigned>(std::countl_zero(v)); // e >= kSubBits
        return static_cast<std::size_t>((e - kSubBits + 1) * kSub + ((v >> (e - kSubBits)) & (kSub - 1)));
    }

    // Largest value that lands in bucket i.
    static std::uint64_t upper_bound(std::size_t i) {
        if (i < kSub) return i;
        const unsigned e = static_cast<unsigned>(i / kSub) + kSubBits - 1;
        const std::uint64_t sub = i % kSub;
        const std::uint64_t lower = (kSub + sub) << (e - kSubBits);
        return lower + (std::uint64_t{1} << (e - kSubBits)) - 1;
    }

    void record(std::uint64_t v) {
        ++counts_[index(v)];
        ++n_;
        sum_ += v;
        if (v > max_) max_ = v;
    }

    // Upper bound of the bucket holding the q-quantile (q in (0, 1]); never above max.
    std::uint64_t percentile(double q) const {
        if (n_ == 0) return 0;
        // rank = ceil(q * n): rounding down would drop the outlier that the tail is about
        auto target = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(n_)));
        if (target < 1) target = 1;
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= target) return upper_bound(i) < max_ ? upper_bound(i) : max_;
        }
        return max_;
    }

    void merge(const Histogram& o) {
        for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        n_ += o.n_;
        sum_ += o.sum_;
        if (o.max_ > max_) max_ = o.max_;
    }

    std::uint64_t count() const { return n_; }
    std::uint64_t max() const { return max_; }
    double mean() const { return n_ ? static_cast<double>(sum_) / static_cast<double>(n_) : 0.0; }
    std::uint64_t bucket(std::size_t i) const { return counts_[i]; }

private:
    std::array<std::uint64_t, kBuckets> counts_{};
    std::uint64_t n_ = 0;
    std::uint64_t sum_ = 0;
    std::uint64_t max_ = 0;
};

} // namespace itch

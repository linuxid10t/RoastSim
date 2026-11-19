// Deterministic PRNG (xoshiro256**) with our own distributions.
// std::normal_distribution etc. are implementation-defined, so a seed would give
// different roasts on libstdc++ (Linux/Haiku/MinGW) vs MSVC. This does not.
#pragma once

#include <cmath>
#include <cstdint>

namespace roastsim {

class Rng {
public:
    explicit Rng(std::uint64_t seed = 0x5EEDC0FFEEULL) { reseed(seed); }

    void reseed(std::uint64_t seed) {
        std::uint64_t x = seed;
        for (auto& s : s_) s = splitmix(x);
        haveSpare_ = false;
    }

    std::uint64_t next() {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform in [0, 1).
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

    // Standard normal (Marsaglia polar method).
    double normal() {
        if (haveSpare_) {
            haveSpare_ = false;
            return spare_;
        }
        double u, v, s;
        do {
            u = uniform() * 2.0 - 1.0;
            v = uniform() * 2.0 - 1.0;
            s = u * u + v * v;
        } while (s >= 1.0 || s == 0.0);
        double m = std::sqrt(-2.0 * std::log(s) / s);
        spare_ = v * m;
        haveSpare_ = true;
        return u * m;
    }

    double normal(double mean, double sd) { return mean + sd * normal(); }

    // Poisson-distributed count for small lambda (Knuth); large lambda uses normal approx.
    int poisson(double lambda) {
        if (lambda <= 0) return 0;
        if (lambda > 30) {
            double v = std::round(normal(lambda, std::sqrt(lambda)));
            return v < 0 ? 0 : static_cast<int>(v);
        }
        double L = std::exp(-lambda), p = 1.0;
        int k = 0;
        do {
            ++k;
            p *= uniform();
        } while (p > L);
        return k - 1;
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    static std::uint64_t splitmix(std::uint64_t& x) {
        std::uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    std::uint64_t s_[4]{};
    double spare_ = 0.0;
    bool haveSpare_ = false;
};

}  // namespace roastsim

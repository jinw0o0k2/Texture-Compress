#ifndef SCAN_ALGORITHMS_HPP
#define SCAN_ALGORITHMS_HPP

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class ScanAlgorithms {
public:
    static uint64_t getScanlineIndex(uint32_t x, uint32_t y, uint32_t width) {
        return static_cast<uint64_t>(y) * width + x;
    }

    static uint64_t getZOrderIndex(uint32_t x, uint32_t y) {
        auto spreadBits = [](uint32_t value) -> uint64_t {
            uint64_t bits = value;
            bits = (bits | (bits << 16)) & 0x0000FFFF0000FFFFULL;
            bits = (bits | (bits << 8)) & 0x00FF00FF00FF00FFULL;
            bits = (bits | (bits << 4)) & 0x0F0F0F0F0F0F0F0FULL;
            bits = (bits | (bits << 2)) & 0x3333333333333333ULL;
            bits = (bits | (bits << 1)) & 0x5555555555555555ULL;
            return bits;
        };
        return spreadBits(x) | (spreadBits(y) << 1);
    }

    static bool isPowerOfTwo(uint32_t value) {
        return value != 0 && (value & (value - 1)) == 0;
    }

    static std::shared_ptr<const std::vector<uint32_t>>
    getZOrderLinearToTargetLut(uint32_t side) {
        static std::mutex cacheMutex;
        static std::unordered_map<
            uint32_t,
            std::shared_ptr<const std::vector<uint32_t>>> cache;

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto found = cache.find(side);
        if (found != cache.end()) return found->second;

        const size_t count = static_cast<size_t>(side) * side;
        auto lut = std::make_shared<std::vector<uint32_t>>(count);
        for (uint32_t y = 0; y < side; ++y) {
            for (uint32_t x = 0; x < side; ++x) {
                size_t linear = static_cast<size_t>(y) * side + x;
                (*lut)[linear] =
                    static_cast<uint32_t>(getZOrderIndex(x, y));
            }
        }
        cache.emplace(side, lut);
        return lut;
    }

    static std::shared_ptr<const std::vector<uint32_t>>
    getZOrderTargetToLinearLut(uint32_t side) {
        static std::mutex cacheMutex;
        static std::unordered_map<
            uint32_t,
            std::shared_ptr<const std::vector<uint32_t>>> cache;

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto found = cache.find(side);
        if (found != cache.end()) return found->second;

        auto linearToTarget = getZOrderLinearToTargetLut(side);
        auto lut = std::make_shared<std::vector<uint32_t>>(
            linearToTarget->size());
        for (std::size_t linear = 0; linear < linearToTarget->size(); ++linear) {
            (*lut)[(*linearToTarget)[linear]] =
                static_cast<uint32_t>(linear);
        }
        cache.emplace(side, lut);
        return lut;
    }

    static void rotateHilbert(uint32_t n,
                              uint32_t* x,
                              uint32_t* y,
                              uint32_t rx,
                              uint32_t ry) {
        if (ry == 0) {
            if (rx == 1) {
                *x = n - 1 - *x;
                *y = n - 1 - *y;
            }
            std::swap(*x, *y);
        }
    }

    static uint64_t getHilbertIndex(uint32_t n, uint32_t x, uint32_t y) {
        uint64_t distance = 0;
        for (uint32_t scale = n / 2; scale > 0; scale /= 2) {
            uint32_t rx = (x & scale) != 0;
            uint32_t ry = (y & scale) != 0;
            distance += static_cast<uint64_t>(scale) * scale * ((3 * rx) ^ ry);
            rotateHilbert(scale, &x, &y, rx, ry);
        }
        return distance;
    }

    static uint64_t getHilbertIndexForRect(uint32_t width,
                                           uint32_t height,
                                           uint32_t x,
                                           uint32_t y) {
        uint32_t side = 1;
        uint32_t required = std::max(width, height);
        while (side < required && side <= (UINT32_MAX >> 1)) {
            side <<= 1;
        }
        return getHilbertIndex(side, x, y);
    }

    static std::shared_ptr<const std::vector<uint32_t>>
    getHilbertLinearToTargetLut(uint32_t side) {
        static std::mutex cacheMutex;
        static std::unordered_map<
            uint32_t,
            std::shared_ptr<const std::vector<uint32_t>>> cache;

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto found = cache.find(side);
        if (found != cache.end()) return found->second;

        const std::size_t count = static_cast<std::size_t>(side) * side;
        auto lut = std::make_shared<std::vector<uint32_t>>(count);
        for (uint32_t y = 0; y < side; ++y) {
            for (uint32_t x = 0; x < side; ++x) {
                const std::size_t linear =
                    static_cast<std::size_t>(y) * side + x;
                (*lut)[linear] = static_cast<uint32_t>(
                    getHilbertIndex(side, x, y));
            }
        }
        cache.emplace(side, lut);
        return lut;
    }

    static std::shared_ptr<const std::vector<uint32_t>>
    getHilbertTargetToLinearLut(uint32_t side) {
        static std::mutex cacheMutex;
        static std::unordered_map<
            uint32_t,
            std::shared_ptr<const std::vector<uint32_t>>> cache;

        std::lock_guard<std::mutex> lock(cacheMutex);
        auto found = cache.find(side);
        if (found != cache.end()) return found->second;

        auto linearToTarget = getHilbertLinearToTargetLut(side);
        auto lut = std::make_shared<std::vector<uint32_t>>(
            linearToTarget->size());
        for (std::size_t linear = 0; linear < linearToTarget->size(); ++linear) {
            (*lut)[(*linearToTarget)[linear]] =
                static_cast<uint32_t>(linear);
        }
        cache.emplace(side, lut);
        return lut;
    }
};

#endif

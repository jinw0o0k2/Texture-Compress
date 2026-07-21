#ifndef SCAN_ALGORITHMS_HPP
#define SCAN_ALGORITHMS_HPP

#include <algorithm>
#include <cstdint>

class ScanAlgorithms {
public:
    static uint64_t getScanlineIndex(uint32_t x, uint32_t y, uint32_t width) {
        return static_cast<uint64_t>(y) * width + x;
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
};

#endif

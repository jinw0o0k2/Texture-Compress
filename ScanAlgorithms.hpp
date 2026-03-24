#ifndef SCAN_ALGORITHMS_HPP
#define SCAN_ALGORITHMS_HPP

#include <cstdint>
#include <algorithm>
#include <cmath>

using namespace std;

class ScanAlgorithms {
public:
    // 1. Scanline (기본 선형 스캔)
    static uint64_t getScanlineIndex(uint32_t x, uint32_t y, uint32_t width) {
        return (uint64_t)y * width + x;
    }

// 2. Z-order Curve (Morton Index) - 수정됨
    static uint64_t splitBy1(uint32_t a) {
        uint64_t x = a; // 입력값을 그대로 64비트로 확장
        
        // 16비트씩 쪼개기 (0000FFFF 0000FFFF)
        x = (x | (x << 16)) & 0x0000FFFF0000FFFF;
        // 8비트씩 쪼개기 (00FF00FF 00FF00FF)
        x = (x | (x << 8))  & 0x00FF00FF00FF00FF;
        // 4비트씩 쪼개기 (0F0F0F0F 0F0F0F0F)
        x = (x | (x << 4))  & 0x0F0F0F0F0F0F0F0F;
        // 2비트씩 쪼개기 (33333333 33333333)
        x = (x | (x << 2))  & 0x3333333333333333;
        // 1비트씩 쪼개기 (55555555 55555555)
        x = (x | (x << 1))  & 0x5555555555555555;
        
        return x;
    }

    static uint64_t getZOrderIndex(uint32_t x, uint32_t y) {
        return (splitBy1(y) << 1) | splitBy1(x);
    }
    
    // 3. Snake Scan (ㄹ자 스캔)
    static uint64_t getSnakeIndex(uint32_t x, uint32_t y, uint32_t width) {
        if (y % 2 == 0) {
            return (uint64_t)y * width + x;
        } else {
            return (uint64_t)y * width + (width - 1 - x);
        }
    }

    // 4. Zig-zag Scan (대각선 스캔)
    static uint64_t getZigzagIndex(uint32_t x, uint32_t y, uint32_t n) {
        if (x + y < n) {
            return (uint64_t)(x + y) * (x + y + 1) / 2 + ((x + y) % 2 == 0 ? x : y);
        } else {
            uint32_t x_rev = n - 1 - x;
            uint32_t y_rev = n - 1 - y;
            return (uint64_t)n * n - 1 - ((uint64_t)(x_rev + y_rev) * (x_rev + y_rev + 1) / 2 + ((x_rev + y_rev) % 2 == 0 ? x_rev : y_rev));
        }
    }

    // 5. Hilbert Curve (공간 채움 곡선)
    static void rotateHilbert(uint32_t n, uint32_t *x, uint32_t *y, uint32_t rx, uint32_t ry) {
        if (ry == 0) {
            if (rx == 1) {
                *x = n - 1 - *x;
                *y = n - 1 - *y;
            }
            swap(*x, *y);
        }
    }

    static uint64_t getHilbertIndex(uint32_t n, uint32_t x, uint32_t y) {
        uint32_t rx, ry;
        uint64_t d = 0;
        for (uint32_t s = n / 2; s > 0; s /= 2) {
            rx = (x & s) > 0;
            ry = (y & s) > 0;
            d += (uint64_t)s * s * ((3 * rx) ^ ry);
            rotateHilbert(s, &x, &y, rx, ry);
        }
        return d;
    }
};

#endif

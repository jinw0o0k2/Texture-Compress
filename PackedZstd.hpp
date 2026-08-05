#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "zstd.h"

namespace PackedZstd {

inline constexpr std::uint8_t kMagic[8] = {
    'T', 'C', 'Z', 'S', 'T', 'D', 1, 0};
inline constexpr std::size_t kHeaderSize = 24;

inline void WriteU64Le(std::uint8_t* out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

inline std::uint64_t ReadU64Le(const std::uint8_t* in) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(in[i]) << (i * 8);
    }
    return value;
}

inline bool Compress(const std::vector<std::uint8_t>& input,
                     std::vector<std::uint8_t>& output,
                     int level) {
    if (input.empty()) return false;
    const std::size_t bound = ZSTD_compressBound(input.size());
    if (ZSTD_isError(bound)) return false;

    output.resize(kHeaderSize + bound);
    std::memcpy(output.data(), kMagic, sizeof(kMagic));
    WriteU64Le(output.data() + 8, input.size());
    const std::size_t compressedSize = ZSTD_compress(
        output.data() + kHeaderSize, bound,
        input.data(), input.size(), level);
    if (ZSTD_isError(compressedSize)) {
        output.clear();
        return false;
    }
    WriteU64Le(output.data() + 16, compressedSize);
    output.resize(kHeaderSize + compressedSize);
    return true;
}

inline bool Decompress(const std::vector<std::uint8_t>& input,
                       std::vector<std::uint8_t>& output) {
    if (input.size() < kHeaderSize ||
        std::memcmp(input.data(), kMagic, sizeof(kMagic)) != 0) {
        return false;
    }
    const std::uint64_t originalSize64 = ReadU64Le(input.data() + 8);
    const std::uint64_t compressedSize64 = ReadU64Le(input.data() + 16);
    if (originalSize64 == 0 || compressedSize64 == 0 ||
        originalSize64 > std::numeric_limits<std::size_t>::max() ||
        compressedSize64 > std::numeric_limits<std::size_t>::max() ||
        compressedSize64 != input.size() - kHeaderSize) {
        return false;
    }

    output.resize(static_cast<std::size_t>(originalSize64));
    const std::size_t decodedSize = ZSTD_decompress(
        output.data(), output.size(), input.data() + kHeaderSize,
        static_cast<std::size_t>(compressedSize64));
    if (ZSTD_isError(decodedSize) || decodedSize != output.size()) {
        output.clear();
        return false;
    }
    return true;
}

} // namespace PackedZstd

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "lz4.h"

namespace PackedLz4 {

inline constexpr std::uint8_t kMagic[8] = {
    'T', 'C', 'L', 'Z', '4', 1, 0, 0};
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
                     std::vector<std::uint8_t>& output) {
    if (input.empty() ||
        input.size() > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE)) {
        return false;
    }
    const int sourceSize = static_cast<int>(input.size());
    const int bound = LZ4_compressBound(sourceSize);
    if (bound <= 0) return false;

    output.resize(kHeaderSize + static_cast<std::size_t>(bound));
    std::memcpy(output.data(), kMagic, sizeof(kMagic));
    WriteU64Le(output.data() + 8, input.size());

    const int compressedSize = LZ4_compress_default(
        reinterpret_cast<const char*>(input.data()),
        reinterpret_cast<char*>(output.data() + kHeaderSize),
        sourceSize,
        bound);
    if (compressedSize <= 0) {
        output.clear();
        return false;
    }
    WriteU64Le(output.data() + 16,
               static_cast<std::uint64_t>(compressedSize));
    output.resize(kHeaderSize + static_cast<std::size_t>(compressedSize));
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
        originalSize64 > static_cast<std::uint64_t>(LZ4_MAX_INPUT_SIZE) ||
        originalSize64 > static_cast<std::uint64_t>(
                             std::numeric_limits<int>::max()) ||
        compressedSize64 > static_cast<std::uint64_t>(
                               std::numeric_limits<int>::max()) ||
        compressedSize64 != input.size() - kHeaderSize) {
        return false;
    }

    const int originalSize = static_cast<int>(originalSize64);
    const int compressedSize = static_cast<int>(compressedSize64);
    output.resize(static_cast<std::size_t>(originalSize));
    const int decodedSize = LZ4_decompress_safe(
        reinterpret_cast<const char*>(input.data() + kHeaderSize),
        reinterpret_cast<char*>(output.data()),
        compressedSize,
        originalSize);
    if (decodedSize != originalSize) {
        output.clear();
        return false;
    }
    return true;
}

} // namespace PackedLz4

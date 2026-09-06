#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "ParallelBlocks.hpp"
#include "ScanAlgorithms.hpp"

namespace PreprocessedRestore {

enum class BcFormat {
    BC1,
    BC3,
    BC4,
};

inline bool DetectBcFormat(std::uint32_t fourCC, BcFormat& format) {
    if (fourCC == 0x31545844) {
        format = BcFormat::BC1;
        return true;
    }
    if (fourCC == 0x35545844) {
        format = BcFormat::BC3;
        return true;
    }
    if (fourCC == 0x31495441 || fourCC == 0x55344342) {
        format = BcFormat::BC4;
        return true;
    }
    return false;
}

inline std::size_t BcBlockSize(BcFormat format) {
    return format == BcFormat::BC3 ? 16U : 8U;
}

struct MapInfo {
    std::uint32_t linearIdx = 0;
    std::uint64_t sortKey = 0;

    bool operator<(const MapInfo& other) const {
        if (sortKey != other.sortKey) return sortKey < other.sortKey;
        return linearIdx < other.linearIdx;
    }
};

inline bool ToDds(const std::vector<std::uint8_t>& buffer,
                  std::vector<std::uint8_t>& dds) {
    if (buffer.size() <= 129) return false;

    std::uint8_t header[128];
    std::memcpy(header, buffer.data(), sizeof(header));
    const std::uint8_t methodFlag = buffer[128];
    if (methodFlag > 2) return false;

    std::uint32_t height = 0;
    std::uint32_t width = 0;
    std::uint32_t fourCC = 0;
    std::memcpy(&height, header + 12, sizeof(height));
    std::memcpy(&width, header + 16, sizeof(width));
    std::memcpy(&fourCC, header + 84, sizeof(fourCC));
    if (width == 0 || height == 0) return false;

    BcFormat format = BcFormat::BC1;
    if (!DetectBcFormat(fourCC, format)) return false;
    const std::size_t blockSize = BcBlockSize(format);

    const std::size_t packedPayloadSize = buffer.size() - 129;
    if (packedPayloadSize % blockSize != 0) {
        return false;
    }

    const std::uint32_t blocksW = (width + 3) / 4;
    const std::uint32_t blocksH = (height + 3) / 4;
    const std::size_t blockCount =
        packedPayloadSize / blockSize;
    if (blockCount == 0) return false;

    std::shared_ptr<const std::vector<std::uint32_t>> linearToTarget;
    std::vector<std::uint32_t> legacyLinearToTarget;
    if (methodFlag == 1 || methodFlag == 2) {
        const std::size_t expectedBlocks =
            static_cast<std::size_t>(blocksW) * blocksH;
        if (blocksW == blocksH && ScanAlgorithms::isPowerOfTwo(blocksW) &&
            blockCount == expectedBlocks) {
            linearToTarget = methodFlag == 1
                ? ScanAlgorithms::getHilbertLinearToTargetLut(blocksW)
                : ScanAlgorithms::getZOrderLinearToTargetLut(blocksW);
            if (linearToTarget->size() != blockCount) return false;
        } else if (methodFlag == 1) {
            // Backward compatibility for old rectangular Hilbert archives.
            std::vector<MapInfo> mapping(blockCount);
            for (std::size_t i = 0; i < blockCount; ++i) {
                const std::uint32_t y =
                    static_cast<std::uint32_t>(i / blocksW);
                const std::uint32_t x =
                    static_cast<std::uint32_t>(i % blocksW);
                mapping[i].linearIdx = static_cast<std::uint32_t>(i);
                mapping[i].sortKey = ScanAlgorithms::getHilbertIndexForRect(
                    blocksW, blocksH, x, y);
            }
            std::sort(mapping.begin(), mapping.end());
            legacyLinearToTarget.resize(blockCount);
            for (std::size_t orderedIdx = 0; orderedIdx < blockCount;
                 ++orderedIdx) {
                legacyLinearToTarget[mapping[orderedIdx].linearIdx] =
                    static_cast<std::uint32_t>(orderedIdx);
            }
        } else {
            return false;
        }
    }

    const std::size_t alphaEndpointBase = 129;
    const std::size_t alphaIndexBase =
        alphaEndpointBase + blockCount * 2;
    std::size_t colorEndpointBase = 129;
    if (format == BcFormat::BC3) {
        colorEndpointBase = alphaIndexBase + blockCount * 6;
    }
    const std::size_t colorIndexBase =
        colorEndpointBase + blockCount * 4;

    dds.resize(128 + blockCount * blockSize);
    std::memcpy(dds.data(), header, 128);
    ParallelBlocks::ForRanges(
        blockCount, [&](std::size_t begin, std::size_t end) {
        for (std::size_t linearIdx = begin; linearIdx < end; ++linearIdx) {
            std::size_t orderedIdx = linearIdx;
            if (linearToTarget) {
                orderedIdx = (*linearToTarget)[linearIdx];
            } else if (!legacyLinearToTarget.empty()) {
                orderedIdx = legacyLinearToTarget[linearIdx];
            }

            std::uint8_t* destination =
                dds.data() + 128 + linearIdx * blockSize;
            if (format == BcFormat::BC3) {
                std::memcpy(destination,
                            buffer.data() + alphaEndpointBase + orderedIdx * 2,
                            2);
                std::memcpy(destination + 2,
                            buffer.data() + alphaIndexBase + orderedIdx * 6,
                            6);
                std::memcpy(destination + 8,
                            buffer.data() + colorEndpointBase + orderedIdx * 4,
                            4);
                std::memcpy(destination + 12,
                            buffer.data() + colorIndexBase + orderedIdx * 4,
                            4);
            } else if (format == BcFormat::BC4) {
                std::memcpy(destination,
                            buffer.data() + alphaEndpointBase + orderedIdx * 2,
                            2);
                std::memcpy(destination + 2,
                            buffer.data() + alphaIndexBase + orderedIdx * 6,
                            6);
            } else {
                std::memcpy(destination,
                            buffer.data() + colorEndpointBase + orderedIdx * 4,
                            4);
                std::memcpy(destination + 4,
                            buffer.data() + colorIndexBase + orderedIdx * 4,
                            4);
            }
        }
    });
    return true;
}

} // namespace PreprocessedRestore

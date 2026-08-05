#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ScanAlgorithms.hpp"

namespace PreprocessedRestore {

struct BlockData {
    std::uint16_t c0 = 0, c1 = 0;
    std::uint32_t cIdx = 0;
    std::uint8_t a0 = 0, a1 = 0;
    std::uint64_t aIdx = 0;
};

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

    bool isBC3 = false;
    bool isBC4 = false;
    int blockSize = 8;
    if (fourCC == 0x35545844) {
        isBC3 = true;
        blockSize = 16;
    } else if (fourCC == 0x31495441 || fourCC == 0x55344342) {
        isBC4 = true;
    }

    const std::size_t packedPayloadSize = buffer.size() - 129;
    if (packedPayloadSize % static_cast<std::size_t>(blockSize) != 0) {
        return false;
    }

    const std::uint32_t blocksW = (width + 3) / 4;
    const std::uint32_t blocksH = (height + 3) / 4;
    const std::size_t blockCount =
        packedPayloadSize / static_cast<std::size_t>(blockSize);
    if (blockCount == 0) return false;

    std::vector<BlockData> orderedBlocks(blockCount);
    std::size_t offset = 129;
    if (isBC3 || isBC4) {
        for (std::size_t i = 0; i < blockCount; ++i) {
            orderedBlocks[i].a0 = buffer[offset++];
            orderedBlocks[i].a1 = buffer[offset++];
        }
        for (std::size_t i = 0; i < blockCount; ++i) {
            std::memcpy(&orderedBlocks[i].aIdx, buffer.data() + offset, 6);
            offset += 6;
        }
    }
    if (!isBC4) {
        for (std::size_t i = 0; i < blockCount; ++i) {
            std::memcpy(&orderedBlocks[i].c0, buffer.data() + offset, 2);
            offset += 2;
            std::memcpy(&orderedBlocks[i].c1, buffer.data() + offset, 2);
            offset += 2;
        }
        for (std::size_t i = 0; i < blockCount; ++i) {
            std::memcpy(&orderedBlocks[i].cIdx, buffer.data() + offset, 4);
            offset += 4;
        }
    }
    if (offset != buffer.size()) return false;

    std::vector<BlockData> restoredBlocks(blockCount);
    if (methodFlag == 0) {
        restoredBlocks = std::move(orderedBlocks);
    } else if (methodFlag == 2) {
        const std::size_t expectedBlocks =
            static_cast<std::size_t>(blocksW) * blocksH;
        if (blocksW != blocksH || !ScanAlgorithms::isPowerOfTwo(blocksW) ||
            blockCount != expectedBlocks) {
            return false;
        }
        auto zOrderLut =
            ScanAlgorithms::getZOrderLinearToTargetLut(blocksW);
        if (zOrderLut->size() != blockCount) return false;
        for (std::size_t linearIdx = 0; linearIdx < blockCount; ++linearIdx) {
            restoredBlocks[linearIdx] =
                orderedBlocks[(*zOrderLut)[linearIdx]];
        }
    } else {
        std::vector<MapInfo> mapping(blockCount);
        for (std::size_t i = 0; i < blockCount; ++i) {
            const std::uint32_t y = static_cast<std::uint32_t>(i / blocksW);
            const std::uint32_t x = static_cast<std::uint32_t>(i % blocksW);
            mapping[i].linearIdx = static_cast<std::uint32_t>(i);
            mapping[i].sortKey = ScanAlgorithms::getHilbertIndexForRect(
                blocksW, blocksH, x, y);
        }
        std::sort(mapping.begin(), mapping.end());
        for (std::size_t i = 0; i < blockCount; ++i) {
            restoredBlocks[mapping[i].linearIdx] = orderedBlocks[i];
        }
    }

    dds.resize(128 + blockCount * static_cast<std::size_t>(blockSize));
    std::size_t outOffset = 0;
    std::memcpy(dds.data(), header, 128);
    outOffset += 128;
    for (const auto& block : restoredBlocks) {
        if (isBC3) {
            dds[outOffset++] = block.a0;
            dds[outOffset++] = block.a1;
            std::memcpy(dds.data() + outOffset, &block.aIdx, 6);
            outOffset += 6;
            std::memcpy(dds.data() + outOffset, &block.c0, 2);
            outOffset += 2;
            std::memcpy(dds.data() + outOffset, &block.c1, 2);
            outOffset += 2;
            std::memcpy(dds.data() + outOffset, &block.cIdx, 4);
            outOffset += 4;
        } else if (isBC4) {
            dds[outOffset++] = block.a0;
            dds[outOffset++] = block.a1;
            std::memcpy(dds.data() + outOffset, &block.aIdx, 6);
            outOffset += 6;
        } else {
            std::memcpy(dds.data() + outOffset, &block.c0, 2);
            outOffset += 2;
            std::memcpy(dds.data() + outOffset, &block.c1, 2);
            outOffset += 2;
            std::memcpy(dds.data() + outOffset, &block.cIdx, 4);
            outOffset += 4;
        }
    }
    return outOffset == dds.size();
}

} // namespace PreprocessedRestore

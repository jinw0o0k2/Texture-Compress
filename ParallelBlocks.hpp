#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

namespace ParallelBlocks {

// Small textures stay single-threaded so thread startup does not dominate.
constexpr std::size_t kMinimumParallelBlocks = 131072;
constexpr std::size_t kBlocksPerChunk = 65536;

inline unsigned ConfiguredWorkerLimit() {
    const char* value = std::getenv("TEXTURE_BLOCK_THREADS");
    if (value && *value) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end != value && *end == '\0' && parsed > 0) {
            return static_cast<unsigned>(std::min<unsigned long>(
                parsed, std::numeric_limits<unsigned>::max()));
        }
    }
    return std::max(1U, std::thread::hardware_concurrency());
}

inline unsigned WorkerCountFor(std::size_t blockCount) {
    if (blockCount < kMinimumParallelBlocks) return 1;

    const std::size_t chunkCount =
        (blockCount + kBlocksPerChunk - 1) / kBlocksPerChunk;
    return static_cast<unsigned>(
        std::min<std::size_t>(ConfiguredWorkerLimit(), chunkCount));
}

template <typename Function>
inline void ForRanges(std::size_t blockCount, Function&& function) {
    const unsigned workerCount = WorkerCountFor(blockCount);
    if (workerCount == 1) {
        function(0, blockCount);
        return;
    }

    const std::size_t chunkCount =
        (blockCount + kBlocksPerChunk - 1) / kBlocksPerChunk;
    std::atomic<std::size_t> nextChunk{0};

    auto worker = [&]() {
        while (true) {
            const std::size_t chunk = nextChunk.fetch_add(
                1, std::memory_order_relaxed);
            if (chunk >= chunkCount) return;

            const std::size_t begin = chunk * kBlocksPerChunk;
            const std::size_t end =
                std::min(begin + kBlocksPerChunk, blockCount);
            function(begin, end);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(workerCount - 1);
    for (unsigned i = 1; i < workerCount; ++i) {
        threads.emplace_back(worker);
    }
    worker();
    for (auto& thread : threads) thread.join();
}

} // namespace ParallelBlocks

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "lz4.h"
#include "PackedLz4.hpp"
#include "PackedZstd.hpp"
#include "ParallelBlocks.hpp"
#include "ScanAlgorithms.hpp"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define TEXTURE_X86_SIMD 1
#else
#define TEXTURE_X86_SIMD 0
#endif

using namespace std;
namespace fs = std::filesystem;

enum class BcFormat {
    BC1,
    BC3,
    BC4,
};

bool DetectBcFormat(uint32_t fourCC, BcFormat& format) {
    if (fourCC == 0x31545844) { // DXT1 / BC1
        format = BcFormat::BC1;
        return true;
    }
    if (fourCC == 0x35545844) { // DXT5 / BC3
        format = BcFormat::BC3;
        return true;
    }
    if (fourCC == 0x31495441 || fourCC == 0x55344342) { // ATI1 / BC4U
        format = BcFormat::BC4;
        return true;
    }
    return false;
}

size_t BcBlockSize(BcFormat format) {
    return format == BcFormat::BC3 ? 16U : 8U;
}

static string exe7z = "C:\\Program Files\\7-Zip\\7z.exe";
static string exePigz = "pigz";
static int compressionLevel = 7;
static string archiveMode = "lz4";
static int requestedOrderMethod = -1;
static int simulationSamplePercent = 20;
static string simulationEngine = "diff";

string SimulationEngineLabel(const string& engine) {
    if (engine == "zstd") return "Zstd-1-ST";
    if (engine == "lz4") return "LZ4-default";
    return "Diff";
}

bool IsInProcessCodec(const string& mode) {
    return mode == "lz4" || mode == "lz4hc" || mode == "zstd";
}

string ArchiveExtension(const string& mode) {
    if (mode == "lz4") return ".packed.lz4";
    if (mode == "lz4hc") return ".packed.lz4hc";
    if (mode == "zstd") return ".packed.zst";
    if (mode == "7z") return ".packed.7z";
    return ".packed.zip";
}

string ArchiveCodecLabel(const string& mode, int level) {
    if (mode == "lz4") return "LZ4-default";
    if (mode == "lz4hc") return "LZ4HC-" + to_string(level);
    if (mode == "zstd") {
        return "Zstd-" + to_string(level) + "-MT" +
               to_string(PackedZstd::DefaultWorkerCount());
    }
    return mode;
}

bool CompressPreparedData(const vector<uint8_t>& prepared,
                          vector<uint8_t>& packed,
                          const string& mode,
                          int level) {
    if (mode == "lz4") return PackedLz4::Compress(prepared, packed);
    if (mode == "lz4hc") {
        return PackedLz4::CompressHC(prepared, packed, level);
    }
    if (mode == "zstd") {
        return PackedZstd::Compress(prepared, packed, level);
    }
    return false;
}

bool DecompressPreparedData(const vector<uint8_t>& packed,
                            vector<uint8_t>& prepared,
                            const string& mode) {
    if (mode == "lz4" || mode == "lz4hc") {
        return PackedLz4::Decompress(packed, prepared);
    }
    if (mode == "zstd") {
        return PackedZstd::Decompress(packed, prepared);
    }
    return false;
}

const char* OrderMethodName(int method) {
    if (method == 1) return "Hilbert";
    if (method == 2) return "Z-order";
    return "Scanline";
}

string Quote(const string& value) {
    return "\"" + value + "\"";
}
bool ReadWholeFile(const fs::path& path, vector<uint8_t>& out) {
    FILE* fp = fopen(path.string().c_str(), "rb");
    if (!fp) return false;

    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSize <= 0) {
        fclose(fp);
        return false;
    }

    out.resize(static_cast<size_t>(fileSize));
    size_t readBytes = fread(out.data(), 1, out.size(), fp);
    fclose(fp);
    return readBytes == out.size();
}

bool WriteWholeFile(const fs::path& path, const uint8_t* data, size_t size) {
    FILE* fp = fopen(path.string().c_str(), "wb");
    if (!fp) return false;
    size_t written = fwrite(data, 1, size, fp);
    fclose(fp);
    return written == size;
}

bool BuildOurPreprocessedData(const fs::path& inputPath,
                              vector<uint8_t>& finalData,
                              int& bestMethod,
                              int forcedMethod,
                              vector<long>* simulatedSizes,
                              unsigned candidateMask,
                              int samplePercent,
                              const string& simulationCodec = "diff") {
    if (simulationCodec != "lz4" && simulationCodec != "zstd" &&
        simulationCodec != "diff") {
        return false;
    }
    vector<uint8_t> originBuffer;
    if (!ReadWholeFile(inputPath, originBuffer) || originBuffer.size() <= 128) {
        return false;
    }

    uint8_t header[128];
    memcpy(header, originBuffer.data(), sizeof(header));

    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t fourCC = 0;
    memcpy(&height, header + 12, sizeof(height));
    memcpy(&width, header + 16, sizeof(width));
    memcpy(&fourCC, header + 84, sizeof(fourCC));

    if (width == 0 || height == 0) return false;

    BcFormat format = BcFormat::BC1;
    if (!DetectBcFormat(fourCC, format)) return false;
    const size_t blockSize = BcBlockSize(format);

    uint32_t blocksW = (width + 3) / 4;
    uint32_t blocksH = (height + 3) / 4;
    size_t fileSize = originBuffer.size();
    size_t payloadSize = fileSize - 128;
    if (payloadSize % blockSize != 0) return false;

    size_t blockCount = payloadSize / blockSize;
    if (blockCount == 0 || blockCount > UINT32_MAX) return false;
    const uint8_t* dPtr = originBuffer.data() + 128;

    const size_t bytesPerBlock = blockSize;
    const size_t totalBufferSize = blockCount * bytesPerBlock;
    const size_t expectedTopLevelBlocks =
        static_cast<size_t>(blocksW) * blocksH;
    const bool zOrderEligible =
        blocksW == blocksH &&
        ScanAlgorithms::isPowerOfTwo(blocksW) &&
        blockCount == expectedTopLevelBlocks;

    candidateMask &= 0x7U; // Scanline, Hilbert, and Z-order.
    if (!zOrderEligible) {
        candidateMask = 0x1U;
        if (forcedMethod < 0 || forcedMethod == 1 || forcedMethod == 2) {
            forcedMethod = 0;
        }
    }
    if (forcedMethod > 2) return false;

    shared_ptr<const vector<uint32_t>> zOrderTargetToLinear;
    shared_ptr<const vector<uint32_t>> hilbertTargetToLinear;
    const bool needZOrder = zOrderEligible &&
        (forcedMethod == 2 ||
         (forcedMethod < 0 && (candidateMask & 0x4U) != 0));
    const bool needHilbert = zOrderEligible &&
        (forcedMethod == 1 ||
         (forcedMethod < 0 && (candidateMask & 0x2U) != 0));
    if (needZOrder) {
        zOrderTargetToLinear =
            ScanAlgorithms::getZOrderTargetToLinearLut(blocksW);
        if (zOrderTargetToLinear->size() != blockCount) return false;
    }
    if (needHilbert) {
        hilbertTargetToLinear =
            ScanAlgorithms::getHilbertTargetToLinearLut(blocksW);
        if (hilbertTargetToLinear->size() != blockCount) return false;
    }

    if (samplePercent != 100 &&
        samplePercent != 20 &&
        samplePercent != 10) {
        return false;
    }
    const size_t sampleCount =
        samplePercent == 100
            ? blockCount
            : max<size_t>(
                  1, (blockCount * static_cast<size_t>(samplePercent) + 99) /
                         100);
    auto LinearIndexAt = [&](int method, size_t orderedIdx) -> size_t {
        if (method == 1) return (*hilbertTargetToLinear)[orderedIdx];
        if (method == 2) return (*zOrderTargetToLinear)[orderedIdx];
        return orderedIdx;
    };

    auto FillSampleBuffer = [&](int method, uint8_t* destination) -> size_t {
        const size_t chunkSize =
            max<size_t>(1, min<size_t>(64, blockCount / 16));
        const size_t totalChunks =
            (blockCount + chunkSize - 1) / chunkSize;
        const size_t sampledChunks = min(
            totalChunks,
            max<size_t>(1,
                (totalChunks * static_cast<size_t>(samplePercent) + 99) /
                    100));

        size_t outBlockIdx = 0;
        for (size_t chunk = 0;
             chunk < sampledChunks && outBlockIdx < sampleCount;
             ++chunk) {
            const size_t chunkIndex = sampledChunks == 1
                ? 0
                : chunk * (totalChunks - 1) / (sampledChunks - 1);
            const size_t startBlock = chunkIndex * chunkSize;
            const size_t endBlock = min(blockCount, startBlock + chunkSize);
            for (size_t orderedIdx = startBlock;
                 orderedIdx < endBlock && outBlockIdx < sampleCount;
                 ++orderedIdx) {
                const size_t sourceIdx = LinearIndexAt(method, orderedIdx);
                memcpy(destination + outBlockIdx * blockSize,
                       dPtr + sourceIdx * blockSize, blockSize);
                ++outBlockIdx;
            }
        }
        return outBlockIdx;
    };

    auto Load16 = [](const uint8_t* source) -> uint16_t {
        uint16_t value = 0;
        memcpy(&value, source, sizeof(value));
        return value;
    };
    auto Load32 = [](const uint8_t* source) -> uint32_t {
        uint32_t value = 0;
        memcpy(&value, source, sizeof(value));
        return value;
    };
    auto Load48 = [](const uint8_t* source) -> uint64_t {
        uint64_t value = 0;
        memcpy(&value, source, 6);
        return value;
    };

    using SimulationScore = uint64_t;
    constexpr SimulationScore kSimulationFailure =
        numeric_limits<SimulationScore>::max();

    auto CalculateDiffScore = [&](int method) -> SimulationScore {
        const size_t simulationBufferSize = sampleCount * bytesPerBlock;
        unique_ptr<uint8_t[]> simBuf(new uint8_t[simulationBufferSize]);
        const size_t sampledBlocks = FillSampleBuffer(method, simBuf.get());
        if (sampledBlocks < 2) return 0;

        SimulationScore totalCost = 0;
        for (size_t i = 1; i < sampledBlocks; ++i) {
            const uint8_t* current = simBuf.get() + i * blockSize;
            const uint8_t* previous = current - blockSize;
            if (format == BcFormat::BC3) {
                totalCost += static_cast<SimulationScore>(abs(
                    static_cast<int>(Load16(current)) -
                    static_cast<int>(Load16(previous))));
                totalCost += Load48(current + 2) != Load48(previous + 2)
                    ? 4U : 0U;
                const uint32_t currentColor = Load32(current + 8);
                const uint32_t previousColor = Load32(previous + 8);
                totalCost += currentColor >= previousColor
                    ? currentColor - previousColor
                    : previousColor - currentColor;
                totalCost += Load32(current + 12) != Load32(previous + 12)
                    ? 4U : 0U;
            } else if (format == BcFormat::BC4) {
                totalCost += static_cast<SimulationScore>(abs(
                    static_cast<int>(Load16(current)) -
                    static_cast<int>(Load16(previous))));
                totalCost += Load48(current + 2) != Load48(previous + 2)
                    ? 4U : 0U;
            } else {
                const uint32_t currentColor = Load32(current);
                const uint32_t previousColor = Load32(previous);
                totalCost += currentColor >= previousColor
                    ? currentColor - previousColor
                    : previousColor - currentColor;
                totalCost += Load32(current + 4) != Load32(previous + 4)
                    ? 4U : 0U;
            }
        }
        return totalCost;
    };

    auto CalculateCompressedSize = [&](int method) -> SimulationScore {
        const size_t simulationBufferSize = sampleCount * bytesPerBlock;
        unique_ptr<uint8_t[]> simBuf(new uint8_t[simulationBufferSize]);
        const size_t sampledBlocks = FillSampleBuffer(method, simBuf.get());
        const size_t actualSampleBytes = sampledBlocks * blockSize;
        if (actualSampleBytes == 0) return kSimulationFailure;

        if (simulationCodec == "zstd") {
            const size_t compressedCapacity =
                ZSTD_compressBound(actualSampleBytes);
            if (ZSTD_isError(compressedCapacity)) return kSimulationFailure;
            vector<uint8_t> compressedBuf(compressedCapacity);
            const size_t compressedSize = ZSTD_compress(
                compressedBuf.data(), compressedCapacity,
                simBuf.get(), actualSampleBytes, 1);
            return ZSTD_isError(compressedSize)
                ? kSimulationFailure
                : static_cast<SimulationScore>(compressedSize);
        }

        if (actualSampleBytes > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) {
            return kSimulationFailure;
        }
        const int sourceSize = static_cast<int>(actualSampleBytes);
        const int compressedCapacity = LZ4_compressBound(sourceSize);
        if (compressedCapacity <= 0) return kSimulationFailure;
        unique_ptr<char[]> compressedBuf(new char[compressedCapacity]);
        const int compressedSize = LZ4_compress_default(
            reinterpret_cast<const char*>(simBuf.get()),
            compressedBuf.get(), sourceSize, compressedCapacity);
        return compressedSize > 0
            ? static_cast<SimulationScore>(compressedSize)
            : kSimulationFailure;
    };

    if (forcedMethod >= 0) {
        bestMethod = forcedMethod;
        if (simulatedSizes) simulatedSizes->clear();
    } else {
        if (candidateMask == 0) return false;

        vector<future<SimulationScore>> futures(3);
        vector<SimulationScore> scores(3, kSimulationFailure);
        for (int method = 0; method < 3; ++method) {
            if ((candidateMask & (1U << method)) != 0) {
                if (simulationCodec == "diff") {
                    futures[method] =
                        async(launch::async, CalculateDiffScore, method);
                } else {
                    futures[method] =
                        async(launch::async, CalculateCompressedSize, method);
                }
            }
        }

        bestMethod = -1;
        for (int method = 0; method < 3; ++method) {
            if ((candidateMask & (1U << method)) == 0) continue;
            scores[method] = futures[method].get();
            if (scores[method] == kSimulationFailure) return false;
            if (bestMethod < 0 || scores[method] < scores[bestMethod]) {
                bestMethod = method;
            }
        }
        if (simulatedSizes) {
            simulatedSizes->assign(3, 0);
            for (int method = 0; method < 3; ++method) {
                if ((candidateMask & (1U << method)) == 0) continue;
                (*simulatedSizes)[method] = static_cast<long>(min<SimulationScore>(
                    scores[method], static_cast<SimulationScore>(LONG_MAX)));
            }
        }
    }

    auto SourceBlockAt = [&](size_t orderedIdx) -> const uint8_t* {
        const size_t sourceIdx = LinearIndexAt(bestMethod, orderedIdx);
        return dPtr + sourceIdx * blockSize;
    };

    finalData.resize(128 + 1 + totalBufferSize);
    memcpy(finalData.data(), header, 128);
    finalData[128] = static_cast<uint8_t>(bestMethod);

    size_t alphaEndpointBase = 129;
    size_t alphaIndexBase = alphaEndpointBase + blockCount * 2;
    size_t colorEndpointBase = 129;
    if (format == BcFormat::BC3) {
        colorEndpointBase = alphaIndexBase + blockCount * 6;
    }
    const size_t colorIndexBase = colorEndpointBase + blockCount * 4;

    ParallelBlocks::ForRanges(blockCount, [&](size_t begin, size_t end) {
        uint8_t* const alphaEndpoint = finalData.data() + alphaEndpointBase;
        uint8_t* const alphaIndex = finalData.data() + alphaIndexBase;
        uint8_t* const colorEndpoint = finalData.data() + colorEndpointBase;
        uint8_t* const colorIndex = finalData.data() + colorIndexBase;
        for (size_t i = begin; i < end; ++i) {
            const uint8_t* source = SourceBlockAt(i);
            if (format == BcFormat::BC3) {
#if TEXTURE_X86_SIMD
                const __m128i block = _mm_loadu_si128(
                    reinterpret_cast<const __m128i*>(source));
                const uint64_t alpha =
                    static_cast<uint64_t>(_mm_cvtsi128_si64(block));
                const __m128i upper = _mm_srli_si128(block, 8);
                const uint64_t color =
                    static_cast<uint64_t>(_mm_cvtsi128_si64(upper));
                memcpy(alphaEndpoint + i * 2, &alpha, 2);
                const uint64_t alphaIndices = alpha >> 16;
                memcpy(alphaIndex + i * 6, &alphaIndices, 6);
                memcpy(colorEndpoint + i * 4, &color, 4);
                const uint64_t colorIndices = color >> 32;
                memcpy(colorIndex + i * 4, &colorIndices, 4);
#else
                const uint16_t endpoint = Load16(source);
                const uint64_t indices = Load48(source + 2);
                memcpy(alphaEndpoint + i * 2, &endpoint, 2);
                memcpy(alphaIndex + i * 6, &indices, 6);
                const uint32_t colorEndpointValue = Load32(source + 8);
                const uint32_t colorIndexValue = Load32(source + 12);
                memcpy(colorEndpoint + i * 4, &colorEndpointValue, 4);
                memcpy(colorIndex + i * 4, &colorIndexValue, 4);
#endif
            } else if (format == BcFormat::BC4) {
                const uint16_t endpoint = Load16(source);
                const uint64_t indices = Load48(source + 2);
                memcpy(alphaEndpoint + i * 2, &endpoint, 2);
                memcpy(alphaIndex + i * 6, &indices, 6);
            } else {
                const uint32_t endpoint = Load32(source);
                const uint32_t indices = Load32(source + 4);
                memcpy(colorEndpoint + i * 4, &endpoint, 4);
                memcpy(colorIndex + i * 4, &indices, 4);
            }
        }
    });

    return true;
}

#undef TEXTURE_X86_SIMD

bool BuildOurPreprocessedBin(const fs::path& inputPath,
                             const fs::path& outputBinPath,
                             int& bestMethod,
                             int forcedMethod = -1,
                             vector<long>* simulatedSizes = nullptr,
                             unsigned candidateMask = 0x7U,
                             int samplePercent = 100,
                             const string& simulationCodec = "diff") {
    vector<uint8_t> finalData;
    if (!BuildOurPreprocessedData(
            inputPath, finalData, bestMethod, forcedMethod,
            simulatedSizes, candidateMask, samplePercent,
            simulationCodec)) {
        return false;
    }
    return WriteWholeFile(
        outputBinPath, finalData.data(), finalData.size());
}

bool ConfigurePigz(const fs::path& programPath) {
    if (const char* configured = getenv("PIGZ_EXE")) {
        if (*configured != '\0' && fs::exists(configured)) {
            exePigz = fs::absolute(configured).string();
            return true;
        }
    }

    error_code ec;
    fs::path besideProgram = fs::absolute(programPath, ec).parent_path() / "pigz.exe";
    if (!ec && fs::exists(besideProgram)) {
        exePigz = besideProgram.string();
        return true;
    }
    if (fs::exists("pigz.exe")) {
        exePigz = fs::absolute("pigz.exe").string();
        return true;
    }

    return system("where pigz > NUL 2>&1") == 0;
}

bool CompressWithPigz(const fs::path& inputBin, const fs::path& archivePath) {
    error_code ec;
    fs::path absoluteInput = fs::absolute(inputBin);
    fs::path absoluteArchive = fs::absolute(archivePath);
    fs::path generatedArchive = absoluteInput;
    generatedArchive += ".zip";

    fs::remove(absoluteArchive, ec);
    ec.clear();
    fs::remove(generatedArchive, ec);

    fs::path previousDirectory = fs::current_path(ec);
    if (ec) return false;
    fs::current_path(absoluteInput.parent_path(), ec);
    if (ec) return false;

    string cmd = Quote(exePigz) + " -K -" + to_string(compressionLevel) +
                 " -k -f " + Quote(absoluteInput.filename().string()) +
                 " > NUL 2>&1";
    int result = system(("\"" + cmd + "\"").c_str());

    error_code restoreError;
    fs::current_path(previousDirectory, restoreError);
    if (result != 0 || restoreError || !fs::exists(generatedArchive)) return false;

    fs::rename(generatedArchive, absoluteArchive, ec);
    return !ec && fs::exists(absoluteArchive);
}

bool CompressPreparedBin(const fs::path& inputBin, const fs::path& archivePath) {
    if (IsInProcessCodec(archiveMode)) {
        vector<uint8_t> prepared;
        vector<uint8_t> packed;
        if (!ReadWholeFile(inputBin, prepared) ||
            !CompressPreparedData(
                prepared, packed, archiveMode, compressionLevel)) {
            return false;
        }
        return WriteWholeFile(archivePath, packed.data(), packed.size());
    }
    if (archiveMode != "7z") return CompressWithPigz(inputBin, archivePath);

    error_code ec;
    fs::remove(archivePath, ec);

    string cmd = Quote(exe7z) + " a -t7z" +
                 " -mx=" + to_string(compressionLevel) + " -bd -y " +
                 Quote(fs::absolute(archivePath).string()) + " " +
                 Quote(fs::absolute(inputBin).string()) + " > NUL 2>&1";
    return system(("\"" + cmd + "\"").c_str()) == 0 && fs::exists(archivePath);
}

bool EncodeFile(const fs::path& inputPath,
                const fs::path& outputRoot,
                const fs::path& relativePath) {
    string extension = ArchiveExtension(archiveMode);
    fs::path archivePath = outputRoot / relativePath;
    archivePath += extension;

    error_code ec;
    fs::create_directories(archivePath.parent_path(), ec);
    if (ec) return false;

    int bestMethod = 0;
    bool ok = false;
    if (IsInProcessCodec(archiveMode)) {
        vector<uint8_t> prepared;
        vector<uint8_t> packed;
        ok = BuildOurPreprocessedData(
                 inputPath, prepared, bestMethod,
                 requestedOrderMethod, nullptr, 0x7U,
                 simulationSamplePercent, simulationEngine) &&
             CompressPreparedData(
                 prepared, packed, archiveMode, compressionLevel) &&
             WriteWholeFile(archivePath, packed.data(), packed.size());
    } else {
        fs::path tempBin = archivePath;
        tempBin += ".tmp.bin";
        ok = BuildOurPreprocessedBin(
                 inputPath, tempBin, bestMethod,
                 requestedOrderMethod, nullptr, 0x7U,
                 simulationSamplePercent, simulationEngine) &&
             CompressPreparedBin(tempBin, archivePath);
        fs::remove(tempBin, ec);
    }

    if (ok) {
        cout << "Encoded: " << inputPath.string()
             << " -> " << archivePath.string()
             << " [" << OrderMethodName(bestMethod) << "]\n";
    }
    return ok;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: " << argv[0]
             << " <input.dds|folder> [output_folder]"
                " [lz4|lz4hc|zstd|pigz|7z] [level]"
                " [auto|scanline|hilbert|zorder] [sample=20|10|100]"
                " [simulation=lz4|zstd|diff]\n";
        return 1;
    }

    fs::path inputPath = fs::absolute(argv[1]);
    fs::path outputRoot = argc >= 3
        ? fs::absolute(argv[2])
        : fs::absolute("Compressed_File");

    if (argc >= 4) {
        archiveMode = argv[3];
        if (archiveMode == "zip") archiveMode = "pigz"; // Backward-compatible alias.
        if (!IsInProcessCodec(archiveMode) && archiveMode != "pigz" &&
            archiveMode != "7z") {
            cerr << "Archive mode must be lz4, lz4hc, zstd, pigz, or 7z.\n";
            return 1;
        }
    }
    if (argc < 5) {
        if (archiveMode == "lz4hc") compressionLevel = 3;
        else if (archiveMode == "zstd") compressionLevel = 1;
    }
    if (argc >= 5) {
        try {
            compressionLevel = stoi(argv[4]);
        } catch (...) {
            cerr << "Compression level must be an integer.\n";
            return 1;
        }
        if (archiveMode == "pigz" && compressionLevel != 7 &&
            compressionLevel != 8 && compressionLevel != 9) {
            cerr << "pigz level must be 7, 8, or 9.\n";
            return 1;
        }
        if (archiveMode == "7z" &&
            (compressionLevel < 0 || compressionLevel > 9)) {
            cerr << "7z level must be from 0 to 9.\n";
            return 1;
        }
        if (archiveMode == "lz4hc" &&
            (compressionLevel < LZ4HC_CLEVEL_MIN ||
             compressionLevel > LZ4HC_CLEVEL_MAX)) {
            cerr << "LZ4HC level must be from " << LZ4HC_CLEVEL_MIN
                 << " to " << LZ4HC_CLEVEL_MAX << ".\n";
            return 1;
        }
        if (archiveMode == "zstd" &&
            (compressionLevel < ZSTD_minCLevel() ||
             compressionLevel > ZSTD_maxCLevel())) {
            cerr << "Zstd level must be from " << ZSTD_minCLevel()
                 << " to " << ZSTD_maxCLevel() << ".\n";
            return 1;
        }
    }
    if (argc >= 6) {
        string order = argv[5];
        if (order == "auto") requestedOrderMethod = -1;
        else if (order == "scanline") requestedOrderMethod = 0;
        else if (order == "hilbert") requestedOrderMethod = 1;
        else if (order == "zorder") requestedOrderMethod = 2;
        else {
            cerr << "Order must be auto, scanline, hilbert, or zorder.\n";
            return 1;
        }
    }
    if (argc >= 7) {
        try {
            simulationSamplePercent = stoi(argv[6]);
        } catch (...) {
            cerr << "Sample percent must be 100, 20, or 10.\n";
            return 1;
        }
        if (simulationSamplePercent != 100 &&
            simulationSamplePercent != 20 &&
            simulationSamplePercent != 10) {
            cerr << "Sample percent must be 100, 20, or 10.\n";
            return 1;
        }
    }
    if (argc >= 8) {
        simulationEngine = argv[7];
        if (simulationEngine != "lz4" && simulationEngine != "zstd" &&
            simulationEngine != "diff") {
            cerr << "Simulation engine must be lz4, zstd, or diff.\n";
            return 1;
        }
    }
    if (!fs::exists(inputPath)) {
        cerr << "Input does not exist: " << inputPath.string() << '\n';
        return 1;
    }
    if (archiveMode == "7z" && !fs::exists(exe7z)) {
        cerr << "7-Zip was not found at C:\\Program Files\\7-Zip\\7z.exe\n";
        return 1;
    }
    if (archiveMode == "pigz" && !ConfigurePigz(argv[0])) {
        cerr << "pigz was not found. Put pigz.exe beside encoder.exe, add it to PATH,\n"
             << "or set PIGZ_EXE to its full path.\n";
        return 1;
    }

    size_t succeeded = 0;
    size_t failed = 0;

    if (fs::is_regular_file(inputPath)) {
        if (inputPath.extension() != ".dds") {
            cerr << "Input file must have the .dds extension.\n";
            return 1;
        }
        EncodeFile(inputPath, outputRoot, inputPath.filename())
            ? ++succeeded : ++failed;
    } else if (fs::is_directory(inputPath)) {
        for (const auto& entry : fs::recursive_directory_iterator(inputPath)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".dds") continue;
            if (entry.path().filename().string().find("_restored") != string::npos) continue;

            error_code ec;
            fs::path relativePath = fs::relative(entry.path(), inputPath, ec);
            if (ec) relativePath = entry.path().filename();
            EncodeFile(entry.path(), outputRoot, relativePath)
                ? ++succeeded : ++failed;
        }
    } else {
        cerr << "Input must be a DDS file or a folder.\n";
        return 1;
    }

    cout << "Finished. Succeeded: " << succeeded << ", Failed: " << failed << '\n';
    return failed == 0 ? 0 : 2;
}

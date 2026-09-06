#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "lz4.h"
#include "PackedLz4.hpp"
#include "PackedZstd.hpp"
#include "ParallelBlocks.hpp"
#include "ScanAlgorithms.hpp"

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
static string simulationEngine = "lz4";

string SimulationEngineLabel(const string& engine) {
    return engine == "zstd" ? "Zstd-1-ST" : "LZ4-default";
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
                              const string& simulationCodec = "lz4") {
    if (simulationCodec != "lz4" && simulationCodec != "zstd") {
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

    auto CalculateSizeInMemory = [&](int method) -> long {
        const size_t simulationBufferSize = sampleCount * bytesPerBlock;
        unique_ptr<uint8_t[]> simBuf(new uint8_t[simulationBufferSize]);
        for (size_t sampleIdx = 0; sampleIdx < sampleCount; ++sampleIdx) {
            const size_t orderedIdx =
                sampleIdx * blockCount / sampleCount;
            const size_t sourceIdx = LinearIndexAt(method, orderedIdx);
            memcpy(simBuf.get() + sampleIdx * blockSize,
                   dPtr + sourceIdx * blockSize, blockSize);
        }

        if (simulationCodec == "zstd") {
            const size_t compressedCapacity =
                ZSTD_compressBound(simulationBufferSize);
            if (ZSTD_isError(compressedCapacity)) return 0;
            vector<uint8_t> compressedBuf(compressedCapacity);
            const size_t compressedSize = ZSTD_compress(
                compressedBuf.data(), compressedCapacity,
                simBuf.get(), simulationBufferSize, 1);
            if (ZSTD_isError(compressedSize) ||
                compressedSize > static_cast<size_t>(LONG_MAX)) {
                return 0;
            }
            return static_cast<long>(compressedSize);
        }

        if (simulationBufferSize > static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) {
            return 0;
        }
        const int sourceSize = static_cast<int>(simulationBufferSize);
        const int compressedCapacity = LZ4_compressBound(sourceSize);
        if (compressedCapacity <= 0) return 0;
        unique_ptr<char[]> compressedBuf(new char[compressedCapacity]);
        const int compressedSize = LZ4_compress_default(
            reinterpret_cast<const char*>(simBuf.get()),
            compressedBuf.get(), sourceSize, compressedCapacity);
        return compressedSize > 0 ? static_cast<long>(compressedSize) : 0;
    };

    if (forcedMethod >= 0) {
        bestMethod = forcedMethod;
        if (simulatedSizes) simulatedSizes->clear();
    } else {
        if (candidateMask == 0) return false;

        vector<future<long>> futures(3);
        vector<long> sizes(3, 0);
        for (int method = 0; method < 3; ++method) {
            if ((candidateMask & (1U << method)) != 0) {
                futures[method] =
                    async(launch::async, CalculateSizeInMemory, method);
            }
        }

        bestMethod = -1;
        for (int method = 0; method < 3; ++method) {
            if ((candidateMask & (1U << method)) == 0) continue;
            sizes[method] = futures[method].get();
            if (sizes[method] <= 0) return false;
            if (bestMethod < 0 || sizes[method] < sizes[bestMethod]) {
                bestMethod = method;
            }
        }
        if (simulatedSizes) *simulatedSizes = sizes;
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
        for (size_t i = begin; i < end; ++i) {
            const uint8_t* source = SourceBlockAt(i);
            if (format == BcFormat::BC3 || format == BcFormat::BC4) {
                memcpy(finalData.data() + alphaEndpointBase + i * 2,
                       source, 2);
                memcpy(finalData.data() + alphaIndexBase + i * 6,
                       source + 2, 6);
            }
            if (format != BcFormat::BC4) {
                const size_t sourceColorOffset =
                    format == BcFormat::BC3 ? 8U : 0U;
                const uint8_t* colorSource = source + sourceColorOffset;
                memcpy(finalData.data() + colorEndpointBase + i * 4,
                       colorSource, 4);
                memcpy(finalData.data() + colorIndexBase + i * 4,
                       colorSource + 4, 4);
            }
        }
    });

    return true;
}

bool BuildOurPreprocessedBin(const fs::path& inputPath,
                             const fs::path& outputBinPath,
                             int& bestMethod,
                             int forcedMethod = -1,
                             vector<long>* simulatedSizes = nullptr,
                             unsigned candidateMask = 0x7U,
                             int samplePercent = 100,
                             const string& simulationCodec = "lz4") {
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
                " [simulation=lz4|zstd]\n";
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
        if (simulationEngine != "lz4" && simulationEngine != "zstd") {
            cerr << "Simulation engine must be lz4 or zstd.\n";
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

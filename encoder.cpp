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
#include "ScanAlgorithms.hpp"

using namespace std;
namespace fs = std::filesystem;

struct BlockData {
    uint32_t originalLinearIdx;
    uint16_t c0, c1;
    uint32_t c_idx;
    uint8_t a0, a1;
    uint64_t a_idx;
};

static string exe7z = "C:\\Program Files\\7-Zip\\7z.exe";
static string exePigz = "pigz";
static int compressionLevel = 7;
static string archiveMode = "pigz";
static int requestedOrderMethod = -1;
static int simulationSamplePercent = 20;

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

bool BuildOurPreprocessedBin(const fs::path& inputPath,
                             const fs::path& outputBinPath,
                             int& bestMethod,
                             int forcedMethod = -1,
                             vector<long>* simulatedSizes = nullptr,
                             unsigned candidateMask = 0x7U,
                             int samplePercent = 100) {
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

    bool isBC3 = false;
    bool isBC4 = false;
    int blockSize = 8;
    if (fourCC == 0x35545844) { // DXT5 / BC3
        isBC3 = true;
        blockSize = 16;
    } else if (fourCC == 0x31495441 || fourCC == 0x55344342) { // ATI1 / BC4U
        isBC4 = true;
    }

    uint32_t blocksW = (width + 3) / 4;
    uint32_t blocksH = (height + 3) / 4;
    size_t fileSize = originBuffer.size();
    size_t payloadSize = fileSize - 128;
    if (payloadSize % static_cast<size_t>(blockSize) != 0) return false;

    size_t blockCount = payloadSize / static_cast<size_t>(blockSize);
    if (blockCount == 0 || blockCount > UINT32_MAX) return false;
    uint8_t* dPtr = originBuffer.data() + 128;

    vector<BlockData> blocks(blockCount);
    for (size_t linearIdx = 0; linearIdx < blockCount; ++linearIdx) {
        BlockData b{};
        b.originalLinearIdx = static_cast<uint32_t>(linearIdx);
        size_t offset = linearIdx * static_cast<size_t>(blockSize);

        if (isBC3) {
            b.a0 = dPtr[offset];
            b.a1 = dPtr[offset + 1];
            memcpy(&b.a_idx, dPtr + offset + 2, 6);
            memcpy(&b.c0, dPtr + offset + 8, 2);
            memcpy(&b.c1, dPtr + offset + 10, 2);
            memcpy(&b.c_idx, dPtr + offset + 12, 4);
        } else if (isBC4) {
            b.a0 = dPtr[offset];
            b.a1 = dPtr[offset + 1];
            memcpy(&b.a_idx, dPtr + offset + 2, 6);
        } else {
            memcpy(&b.c0, dPtr + offset, 2);
            memcpy(&b.c1, dPtr + offset + 2, 2);
            memcpy(&b.c_idx, dPtr + offset + 4, 4);
        }
        blocks[linearIdx] = b;
    }

    const size_t bytesPerBlock = isBC3 ? 16 : 8;
    const size_t totalBufferSize = blockCount * bytesPerBlock;
    const size_t expectedTopLevelBlocks =
        static_cast<size_t>(blocksW) * blocksH;
    const bool zOrderEligible =
        blocksW == blocksH &&
        ScanAlgorithms::isPowerOfTwo(blocksW) &&
        blockCount == expectedTopLevelBlocks;

    candidateMask &= 0x5U; // Scanline and Z-order only.
    if (!zOrderEligible) {
        candidateMask = 0x1U;
        if (forcedMethod < 0 || forcedMethod == 2) forcedMethod = 0;
    }
    if (forcedMethod == 1 || forcedMethod > 2) return false;

    shared_ptr<const vector<uint32_t>> zOrderLut;
    vector<BlockData> zOrderBlocks;
    const bool needZOrder =
        zOrderEligible &&
        (forcedMethod == 2 ||
         (forcedMethod < 0 && (candidateMask & 0x4U) != 0));
    if (needZOrder) {
        zOrderLut =
            ScanAlgorithms::getZOrderLinearToTargetLut(blocksW);
        if (zOrderLut->size() != blockCount) return false;
        zOrderBlocks.resize(blockCount);
        for (size_t linearIdx = 0; linearIdx < blockCount; ++linearIdx) {
            uint32_t targetIdx = (*zOrderLut)[linearIdx];
            zOrderBlocks[targetIdx] = blocks[linearIdx];
        }
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
    vector<uint8_t> sampleMask;
    if (sampleCount != blockCount) {
        sampleMask.assign(blockCount, 0);
        for (size_t sampleIdx = 0; sampleIdx < sampleCount; ++sampleIdx) {
            size_t linearIdx = sampleIdx * blockCount / sampleCount;
            sampleMask[linearIdx] = 1;
        }
    }

    auto CalculateSizeInMemory = [&](int method) -> long {
        const vector<BlockData>& orderedBlocks =
            method == 2 ? zOrderBlocks : blocks;

        const size_t simulationBufferSize = sampleCount * bytesPerBlock;
        unique_ptr<uint8_t[]> simBuf(new uint8_t[simulationBufferSize]);
        size_t offset = 0;
        for (const auto& b : orderedBlocks) {
            if (!sampleMask.empty() &&
                sampleMask[b.originalLinearIdx] == 0) {
                continue;
            }
            if (isBC3 || isBC4) {
                simBuf[offset++] = b.a0;
                simBuf[offset++] = b.a1;
                memcpy(simBuf.get() + offset, &b.a_idx, 6);
                offset += 6;
            }
            if (!isBC4) {
                memcpy(simBuf.get() + offset, &b.c0, 2);
                offset += 2;
                memcpy(simBuf.get() + offset, &b.c1, 2);
                offset += 2;
                memcpy(simBuf.get() + offset, &b.c_idx, 4);
                offset += 4;
            }
        }
        if (offset != simulationBufferSize) return 0;

        if (simulationBufferSize >
            static_cast<size_t>(LZ4_MAX_INPUT_SIZE)) {
            return 0;
        }
        const int sourceSize =
            static_cast<int>(simulationBufferSize);
        const int compressedCapacity = LZ4_compressBound(sourceSize);
        if (compressedCapacity <= 0) return 0;

        unique_ptr<char[]> compressedBuf(new char[compressedCapacity]);
        const int compressedSize = LZ4_compress_default(
            reinterpret_cast<const char*>(simBuf.get()),
            compressedBuf.get(), sourceSize, compressedCapacity);
        return compressedSize > 0
                   ? static_cast<long>(compressedSize)
                   : 0;
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

    const vector<BlockData>& finalBlocks =
        bestMethod == 2 ? zOrderBlocks : blocks;

    unique_ptr<uint8_t[]> finalBuf(new uint8_t[128 + 1 + totalBufferSize]);
    size_t finalOffset = 0;
    memcpy(finalBuf.get() + finalOffset, header, 128);
    finalOffset += 128;
    finalBuf[finalOffset++] = static_cast<uint8_t>(bestMethod);

    if (isBC3 || isBC4) {
        for (const auto& b : finalBlocks) {
            finalBuf[finalOffset++] = b.a0;
            finalBuf[finalOffset++] = b.a1;
        }
        for (const auto& b : finalBlocks) {
            memcpy(finalBuf.get() + finalOffset, &b.a_idx, 6);
            finalOffset += 6;
        }
    }
    if (!isBC4) {
        for (const auto& b : finalBlocks) {
            memcpy(finalBuf.get() + finalOffset, &b.c0, 2);
            finalOffset += 2;
            memcpy(finalBuf.get() + finalOffset, &b.c1, 2);
            finalOffset += 2;
        }
        for (const auto& b : finalBlocks) {
            memcpy(finalBuf.get() + finalOffset, &b.c_idx, 4);
            finalOffset += 4;
        }
    }

    return WriteWholeFile(outputBinPath, finalBuf.get(), finalOffset);
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
    string extension = archiveMode == "7z" ? ".packed.7z" : ".packed.zip";
    fs::path archivePath = outputRoot / relativePath;
    archivePath += extension;

    error_code ec;
    fs::create_directories(archivePath.parent_path(), ec);
    if (ec) return false;

    fs::path tempBin = archivePath;
    tempBin += ".tmp.bin";

    int bestMethod = 0;
    if (!BuildOurPreprocessedBin(inputPath, tempBin, bestMethod,
                                 requestedOrderMethod, nullptr, 0x5U,
                                 simulationSamplePercent)) {
        fs::remove(tempBin, ec);
        return false;
    }

    bool ok = CompressPreparedBin(tempBin, archivePath);
    fs::remove(tempBin, ec);

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
             << " <input.dds|folder> [output_folder] [pigz|7z] [level=7]"
                " [auto|scanline|zorder] [sample=20|10|100]\n";
        return 1;
    }

    fs::path inputPath = fs::absolute(argv[1]);
    fs::path outputRoot = argc >= 3
        ? fs::absolute(argv[2])
        : fs::absolute("Compressed_File");

    if (argc >= 4) {
        archiveMode = argv[3];
        if (archiveMode == "zip") archiveMode = "pigz"; // Backward-compatible alias.
        if (archiveMode != "pigz" && archiveMode != "7z") {
            cerr << "Archive mode must be pigz or 7z.\n";
            return 1;
        }
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
    }
    if (argc >= 6) {
        string order = argv[5];
        if (order == "auto") requestedOrderMethod = -1;
        else if (order == "scanline") requestedOrderMethod = 0;
        else if (order == "zorder") requestedOrderMethod = 2;
        else {
            cerr << "Order must be auto, scanline, or zorder.\n";
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

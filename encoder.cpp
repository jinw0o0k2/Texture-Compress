#include <algorithm>
#include <cstdio>
#include <cstring>
#include <execution>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "ScanAlgorithms.hpp"
#include "miniz.h"

using namespace std;
namespace fs = std::filesystem;

struct BlockData {
    uint64_t sortKey;
    uint32_t originalLinearIdx;
    uint16_t c0, c1;
    uint32_t c_idx;
    uint8_t a0, a1;
    uint64_t a_idx;

    bool operator<(const BlockData& other) const {
        if (sortKey != other.sortKey) return sortKey < other.sortKey;
        return originalLinearIdx < other.originalLinearIdx;
    }
};

static string exe7z = "\"C:\\Program Files\\7-Zip\\7z.exe\"";
static int compressionLevel = 9;
static string archiveMode = "zip";

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
                             int& bestMethod) {
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
    if (blockCount == 0) return false;
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

    auto CalculateSizeInMemory = [&](int method) -> long {
        vector<uint32_t> idxMap(blockCount);
        iota(idxMap.begin(), idxMap.end(), 0);

        vector<uint64_t> keys(blockCount);
        for (size_t i = 0; i < blockCount; ++i) {
            uint32_t y = static_cast<uint32_t>(i / blocksW);
            uint32_t x = static_cast<uint32_t>(i % blocksW);
            keys[i] = method == 1
                ? ScanAlgorithms::getHilbertIndexForRect(blocksW, blocksH, x, y)
                : ScanAlgorithms::getScanlineIndex(x, y, blocksW);
        }

        sort(execution::par, idxMap.begin(), idxMap.end(), [&](uint32_t a, uint32_t b) {
            if (keys[a] != keys[b]) return keys[a] < keys[b];
            return a < b;
        });

        unique_ptr<uint8_t[]> simBuf(new uint8_t[totalBufferSize]);
        size_t offset = 0;
        for (uint32_t index : idxMap) {
            const auto& b = blocks[index];
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

        unsigned long compressedLen = mz_compressBound(
            static_cast<unsigned long>(totalBufferSize));
        unique_ptr<uint8_t[]> compressedBuf(new uint8_t[compressedLen]);
        int status = mz_compress(compressedBuf.get(), &compressedLen,
                                 simBuf.get(),
                                 static_cast<unsigned long>(totalBufferSize));
        return status == MZ_OK ? static_cast<long>(compressedLen) : 0;
    };

    auto futureScan = async(launch::async, CalculateSizeInMemory, 0);
    auto futureHilbert = async(launch::async, CalculateSizeInMemory, 1);
    long sizeScan = futureScan.get();
    long sizeHilbert = futureHilbert.get();
    if (sizeScan <= 0 || sizeHilbert <= 0) return false;
    bestMethod = sizeHilbert < sizeScan ? 1 : 0;

    for (auto& block : blocks) {
        uint32_t y = block.originalLinearIdx / blocksW;
        uint32_t x = block.originalLinearIdx % blocksW;
        block.sortKey = bestMethod == 1
            ? ScanAlgorithms::getHilbertIndexForRect(blocksW, blocksH, x, y)
            : ScanAlgorithms::getScanlineIndex(x, y, blocksW);
    }
    sort(execution::par, blocks.begin(), blocks.end());

    unique_ptr<uint8_t[]> finalBuf(new uint8_t[128 + 1 + totalBufferSize]);
    size_t finalOffset = 0;
    memcpy(finalBuf.get() + finalOffset, header, 128);
    finalOffset += 128;
    finalBuf[finalOffset++] = static_cast<uint8_t>(bestMethod);

    if (isBC3 || isBC4) {
        for (const auto& b : blocks) {
            finalBuf[finalOffset++] = b.a0;
            finalBuf[finalOffset++] = b.a1;
        }
        for (const auto& b : blocks) {
            memcpy(finalBuf.get() + finalOffset, &b.a_idx, 6);
            finalOffset += 6;
        }
    }
    if (!isBC4) {
        for (const auto& b : blocks) {
            memcpy(finalBuf.get() + finalOffset, &b.c0, 2);
            finalOffset += 2;
            memcpy(finalBuf.get() + finalOffset, &b.c1, 2);
            finalOffset += 2;
        }
        for (const auto& b : blocks) {
            memcpy(finalBuf.get() + finalOffset, &b.c_idx, 4);
            finalOffset += 4;
        }
    }

    return WriteWholeFile(outputBinPath, finalBuf.get(), finalOffset);
}

bool CompressWith7z(const fs::path& inputBin, const fs::path& archivePath) {
    error_code ec;
    fs::remove(archivePath, ec);

    string typeArg = archiveMode == "7z" ? "-t7z" : "-tzip";
    string cmd = "\"" + exe7z + " a " + typeArg +
                 " -mx=" + to_string(compressionLevel) + " -bd -y " +
                 Quote(fs::absolute(archivePath).string()) + " " +
                 Quote(fs::absolute(inputBin).string()) + " > NUL 2>&1\"";
    return system(cmd.c_str()) == 0 && fs::exists(archivePath);
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
    if (!BuildOurPreprocessedBin(inputPath, tempBin, bestMethod)) {
        fs::remove(tempBin, ec);
        return false;
    }

    bool ok = CompressWith7z(tempBin, archivePath);
    fs::remove(tempBin, ec);

    if (ok) {
        cout << "Encoded: " << inputPath.string()
             << " -> " << archivePath.string()
             << " [" << (bestMethod == 1 ? "Hilbert" : "Scanline") << "]\n";
    }
    return ok;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: " << argv[0]
             << " <input.dds|folder> [output_folder] [zip|7z] [level=9]\n";
        return 1;
    }

    fs::path inputPath = fs::absolute(argv[1]);
    fs::path outputRoot = argc >= 3
        ? fs::absolute(argv[2])
        : fs::absolute("Compressed_File");

    if (argc >= 4) {
        archiveMode = argv[3];
        if (archiveMode != "zip" && archiveMode != "7z") {
            cerr << "Archive mode must be zip or 7z.\n";
            return 1;
        }
    }
    if (argc >= 5) {
        try {
            compressionLevel = stoi(argv[4]);
        } catch (...) {
            cerr << "Compression level must be an integer from 0 to 9.\n";
            return 1;
        }
        if (compressionLevel < 0 || compressionLevel > 9) {
            cerr << "Compression level must be from 0 to 9.\n";
            return 1;
        }
    }

    if (!fs::exists(inputPath)) {
        cerr << "Input does not exist: " << inputPath.string() << '\n';
        return 1;
    }
    if (!fs::exists("C:\\Program Files\\7-Zip\\7z.exe")) {
        cerr << "7-Zip was not found at C:\\Program Files\\7-Zip\\7z.exe\n";
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

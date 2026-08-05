#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "PackedLz4.hpp"
#include "PreprocessedRestore.hpp"
#include "ScanAlgorithms.hpp"

using namespace std;
namespace fs = std::filesystem;

struct BlockData {
    uint16_t c0 = 0, c1 = 0;
    uint32_t c_idx = 0;
    uint8_t a0 = 0, a1 = 0;
    uint64_t a_idx = 0;
};

struct MapInfo {
    uint32_t linearIdx = 0;
    uint64_t sortKey = 0;

    bool operator<(const MapInfo& other) const {
        if (sortKey != other.sortKey) return sortKey < other.sortKey;
        return linearIdx < other.linearIdx;
    }
};

static string exe7z = "\"C:\\Program Files\\7-Zip\\7z.exe\"";

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

bool ExtractArchive(const fs::path& archivePath, const fs::path& outDir) {
    error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) return false;

    string cmd = "\"" + exe7z + " x -y -bd " +
                 Quote(fs::absolute(archivePath).string()) + " -o" +
                 Quote(fs::absolute(outDir).string()) + " > NUL 2>&1\"";
    return system(cmd.c_str()) == 0;
}

bool FindExtractedBin(const fs::path& directory, fs::path& binPath) {
    error_code ec;
    for (fs::recursive_directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".bin") {
            binPath = it->path();
            return true;
        }
    }
    return false;
}

bool RestoreOurPreprocessedData(const vector<uint8_t>& buffer,
                                const fs::path& outputPath) {
    if (buffer.size() <= 129) return false;

    uint8_t header[128];
    memcpy(header, buffer.data(), sizeof(header));
    uint8_t methodFlag = buffer[128];
    if (methodFlag > 2) return false;

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

    size_t packedPayloadSize = buffer.size() - 129;
    if (packedPayloadSize % static_cast<size_t>(blockSize) != 0) return false;

    uint32_t blocksW = (width + 3) / 4;
    uint32_t blocksH = (height + 3) / 4;
    size_t blockCount = packedPayloadSize / static_cast<size_t>(blockSize);
    if (blockCount == 0) return false;

    vector<BlockData> sortedBlocks(blockCount);
    size_t offset = 129;

    if (isBC3 || isBC4) {
        for (size_t i = 0; i < blockCount; ++i) {
            sortedBlocks[i].a0 = buffer[offset++];
            sortedBlocks[i].a1 = buffer[offset++];
        }
        for (size_t i = 0; i < blockCount; ++i) {
            memcpy(&sortedBlocks[i].a_idx, buffer.data() + offset, 6);
            offset += 6;
        }
    }

    if (!isBC4) {
        for (size_t i = 0; i < blockCount; ++i) {
            memcpy(&sortedBlocks[i].c0, buffer.data() + offset, 2);
            offset += 2;
            memcpy(&sortedBlocks[i].c1, buffer.data() + offset, 2);
            offset += 2;
        }
        for (size_t i = 0; i < blockCount; ++i) {
            memcpy(&sortedBlocks[i].c_idx, buffer.data() + offset, 4);
            offset += 4;
        }
    }
    if (offset != buffer.size()) return false;

    vector<BlockData> restoredBlocks(blockCount);
    if (methodFlag == 0) {
        restoredBlocks = std::move(sortedBlocks);
    } else if (methodFlag == 2) {
        const size_t expectedTopLevelBlocks =
            static_cast<size_t>(blocksW) * blocksH;
        if (blocksW != blocksH ||
            !ScanAlgorithms::isPowerOfTwo(blocksW) ||
            blockCount != expectedTopLevelBlocks) {
            return false;
        }
        auto zOrderLut =
            ScanAlgorithms::getZOrderLinearToTargetLut(blocksW);
        if (zOrderLut->size() != blockCount) return false;
        for (size_t linearIdx = 0; linearIdx < blockCount; ++linearIdx) {
            restoredBlocks[linearIdx] =
                sortedBlocks[(*zOrderLut)[linearIdx]];
        }
    } else {
        // Backward compatibility for archives produced by the old Hilbert encoder.
        vector<MapInfo> mapping(blockCount);
        for (size_t i = 0; i < blockCount; ++i) {
            uint32_t y = static_cast<uint32_t>(i / blocksW);
            uint32_t x = static_cast<uint32_t>(i % blocksW);
            mapping[i].linearIdx = static_cast<uint32_t>(i);
            mapping[i].sortKey =
                ScanAlgorithms::getHilbertIndexForRect(
                    blocksW, blocksH, x, y);
        }
        sort(mapping.begin(), mapping.end());
        for (size_t i = 0; i < blockCount; ++i) {
            restoredBlocks[mapping[i].linearIdx] = sortedBlocks[i];
        }
    }

    unique_ptr<uint8_t[]> outBuf(new uint8_t[128 + blockCount * blockSize]);
    size_t outOffset = 0;
    memcpy(outBuf.get(), header, 128);
    outOffset += 128;

    for (const auto& b : restoredBlocks) {
        if (isBC3) {
            outBuf[outOffset++] = b.a0;
            outBuf[outOffset++] = b.a1;
            memcpy(outBuf.get() + outOffset, &b.a_idx, 6);
            outOffset += 6;
            memcpy(outBuf.get() + outOffset, &b.c0, 2);
            outOffset += 2;
            memcpy(outBuf.get() + outOffset, &b.c1, 2);
            outOffset += 2;
            memcpy(outBuf.get() + outOffset, &b.c_idx, 4);
            outOffset += 4;
        } else if (isBC4) {
            outBuf[outOffset++] = b.a0;
            outBuf[outOffset++] = b.a1;
            memcpy(outBuf.get() + outOffset, &b.a_idx, 6);
            outOffset += 6;
        } else {
            memcpy(outBuf.get() + outOffset, &b.c0, 2);
            outOffset += 2;
            memcpy(outBuf.get() + outOffset, &b.c1, 2);
            outOffset += 2;
            memcpy(outBuf.get() + outOffset, &b.c_idx, 4);
            outOffset += 4;
        }
    }

    error_code ec;
    fs::create_directories(outputPath.parent_path(), ec);
    return !ec && WriteWholeFile(outputPath, outBuf.get(), outOffset);
}

bool RestoreOurPreprocessedBin(const fs::path& binPath,
                               const fs::path& outputPath) {
    vector<uint8_t> buffer;
    vector<uint8_t> dds;
    if (!ReadWholeFile(binPath, buffer) ||
        !PreprocessedRestore::ToDds(buffer, dds)) {
        return false;
    }
    error_code ec;
    fs::create_directories(outputPath.parent_path(), ec);
    return !ec && WriteWholeFile(outputPath, dds.data(), dds.size());
}

string RemovePackedExtension(string filename) {
    const string zipSuffix = ".packed.zip";
    const string sevenZipSuffix = ".packed.7z";
    const string lz4Suffix = ".packed.lz4";

    if (filename.size() >= lz4Suffix.size() &&
        filename.compare(filename.size() - lz4Suffix.size(),
                         lz4Suffix.size(), lz4Suffix) == 0) {
        filename.erase(filename.size() - lz4Suffix.size());
    } else if (filename.size() >= zipSuffix.size() &&
        filename.compare(filename.size() - zipSuffix.size(), zipSuffix.size(), zipSuffix) == 0) {
        filename.erase(filename.size() - zipSuffix.size());
    } else if (filename.size() >= sevenZipSuffix.size() &&
               filename.compare(filename.size() - sevenZipSuffix.size(), sevenZipSuffix.size(), sevenZipSuffix) == 0) {
        filename.erase(filename.size() - sevenZipSuffix.size());
    } else {
        filename += ".restored.dds";
    }
    return filename;
}

bool IsPackedArchive(const fs::path& path) {
    string name = path.filename().string();
    const string zipSuffix = ".packed.zip";
    const string sevenZipSuffix = ".packed.7z";
    const string lz4Suffix = ".packed.lz4";
    bool isZip = name.size() >= zipSuffix.size() &&
                 name.compare(name.size() - zipSuffix.size(), zipSuffix.size(), zipSuffix) == 0;
    bool is7z = name.size() >= sevenZipSuffix.size() &&
                 name.compare(name.size() - sevenZipSuffix.size(), sevenZipSuffix.size(), sevenZipSuffix) == 0;
    bool isLz4 = name.size() >= lz4Suffix.size() &&
                 name.compare(name.size() - lz4Suffix.size(), lz4Suffix.size(), lz4Suffix) == 0;
    return isZip || is7z || isLz4;
}

bool DecodeFile(const fs::path& archivePath,
                const fs::path& outputRoot,
                const fs::path& relativePath) {
    fs::path outputPath = outputRoot / relativePath.parent_path() /
                          RemovePackedExtension(relativePath.filename().string());

    bool ok = false;
    if (archivePath.extension() == ".lz4") {
        vector<uint8_t> packed;
        vector<uint8_t> prepared;
        vector<uint8_t> dds;
        ok = ReadWholeFile(archivePath, packed) &&
             PackedLz4::Decompress(packed, prepared) &&
             PreprocessedRestore::ToDds(prepared, dds);
        if (ok) {
            error_code ec;
            fs::create_directories(outputPath.parent_path(), ec);
            ok = !ec && WriteWholeFile(
                            outputPath, dds.data(), dds.size());
        }
    } else {
        auto uniqueValue = chrono::high_resolution_clock::now()
                               .time_since_epoch().count();
        fs::path tempDir = fs::temp_directory_path() /
                           ("ourcompress_decode_" + to_string(uniqueValue));
        if (ExtractArchive(archivePath, tempDir)) {
            fs::path binPath;
            if (FindExtractedBin(tempDir, binPath)) {
                ok = RestoreOurPreprocessedBin(binPath, outputPath);
            }
        }
        error_code ec;
        fs::remove_all(tempDir, ec);
    }

    if (ok) {
        cout << "Decoded: " << archivePath.string()
             << " -> " << outputPath.string() << '\n';
    }
    return ok;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: " << argv[0]
             << " <archive|folder> [output_folder]\n";
        return 1;
    }

    fs::path inputPath = fs::absolute(argv[1]);
    fs::path outputRoot = argc >= 3
        ? fs::absolute(argv[2])
        : fs::absolute("Restored_File");

    if (!fs::exists(inputPath)) {
        cerr << "Input does not exist: " << inputPath.string() << '\n';
        return 1;
    }

    size_t succeeded = 0;
    size_t failed = 0;

    if (fs::is_regular_file(inputPath)) {
        DecodeFile(inputPath, outputRoot, inputPath.filename())
            ? ++succeeded : ++failed;
    } else if (fs::is_directory(inputPath)) {
        for (const auto& entry : fs::recursive_directory_iterator(inputPath)) {
            if (!entry.is_regular_file() || !IsPackedArchive(entry.path())) continue;

            error_code ec;
            fs::path relativePath = fs::relative(entry.path(), inputPath, ec);
            if (ec) relativePath = entry.path().filename();
            DecodeFile(entry.path(), outputRoot, relativePath)
                ? ++succeeded : ++failed;
        }
    } else {
        cerr << "Input must be a packed archive or a folder.\n";
        return 1;
    }

    cout << "Finished. Succeeded: " << succeeded << ", Failed: " << failed << '\n';
    return failed == 0 ? 0 : 2;
}

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "PackedLz4.hpp"
#include "PackedZstd.hpp"
#include "PreprocessedRestore.hpp"

using namespace std;
namespace fs = std::filesystem;

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
    const string lz4hcSuffix = ".packed.lz4hc";
    const string zstdSuffix = ".packed.zst";

    if (filename.size() >= lz4hcSuffix.size() &&
        filename.compare(filename.size() - lz4hcSuffix.size(),
                         lz4hcSuffix.size(), lz4hcSuffix) == 0) {
        filename.erase(filename.size() - lz4hcSuffix.size());
    } else if (filename.size() >= zstdSuffix.size() &&
        filename.compare(filename.size() - zstdSuffix.size(),
                         zstdSuffix.size(), zstdSuffix) == 0) {
        filename.erase(filename.size() - zstdSuffix.size());
    } else if (filename.size() >= lz4Suffix.size() &&
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
    const string lz4hcSuffix = ".packed.lz4hc";
    const string zstdSuffix = ".packed.zst";
    bool isZip = name.size() >= zipSuffix.size() &&
                 name.compare(name.size() - zipSuffix.size(), zipSuffix.size(), zipSuffix) == 0;
    bool is7z = name.size() >= sevenZipSuffix.size() &&
                 name.compare(name.size() - sevenZipSuffix.size(), sevenZipSuffix.size(), sevenZipSuffix) == 0;
    bool isLz4 = name.size() >= lz4Suffix.size() &&
                 name.compare(name.size() - lz4Suffix.size(), lz4Suffix.size(), lz4Suffix) == 0;
    bool isLz4hc = name.size() >= lz4hcSuffix.size() &&
                   name.compare(name.size() - lz4hcSuffix.size(), lz4hcSuffix.size(), lz4hcSuffix) == 0;
    bool isZstd = name.size() >= zstdSuffix.size() &&
                  name.compare(name.size() - zstdSuffix.size(), zstdSuffix.size(), zstdSuffix) == 0;
    return isZip || is7z || isLz4 || isLz4hc || isZstd;
}

bool DecodeFile(const fs::path& archivePath,
                const fs::path& outputRoot,
                const fs::path& relativePath) {
    fs::path outputPath = outputRoot / relativePath.parent_path() /
                          RemovePackedExtension(relativePath.filename().string());

    bool ok = false;
    if (archivePath.extension() == ".lz4" ||
        archivePath.extension() == ".lz4hc" ||
        archivePath.extension() == ".zst") {
        vector<uint8_t> packed;
        vector<uint8_t> prepared;
        vector<uint8_t> dds;
        ok = ReadWholeFile(archivePath, packed) &&
             (archivePath.extension() == ".zst"
                  ? PackedZstd::Decompress(packed, prepared)
                  : PackedLz4::Decompress(packed, prepared)) &&
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

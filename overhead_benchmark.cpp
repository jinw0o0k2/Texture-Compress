#define main texture_compress_encoder_main
#include "encoder.cpp"
#undef main

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "PreprocessedRestore.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct FileAverage {
    std::string relativePath;
    std::uintmax_t originalBytes = 0;
    std::uintmax_t compressedBytes = 0;
    double preprocessMs = 0.0;
    double secondaryMs = 0.0;
    double totalEncodeMs = 0.0;
    double decodeCoreMs = 0.0;
    double decodeWriteMs = 0.0;
    double totalDecodeMs = 0.0;
};

double ElapsedMs(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::string Csv(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '"') escaped.push_back('"');
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

bool FilesEqual(const fs::path& left, const fs::path& right) {
    error_code ec;
    if (!fs::exists(left) || !fs::exists(right) ||
        fs::file_size(left, ec) != fs::file_size(right, ec) || ec) {
        return false;
    }

    std::ifstream a(left, std::ios::binary);
    std::ifstream b(right, std::ios::binary);
    if (!a || !b) return false;

    constexpr size_t kBufferSize = 1 << 20;
    std::vector<char> bufferA(kBufferSize);
    std::vector<char> bufferB(kBufferSize);
    while (a && b) {
        a.read(bufferA.data(), static_cast<std::streamsize>(bufferA.size()));
        b.read(bufferB.data(), static_cast<std::streamsize>(bufferB.size()));
        if (a.gcount() != b.gcount() ||
            !std::equal(bufferA.begin(), bufferA.begin() + a.gcount(), bufferB.begin())) {
            return false;
        }
    }
    return true;
}

std::vector<fs::path> CollectDdsFiles(const fs::path& input) {
    std::vector<fs::path> files;
    if (fs::is_regular_file(input)) {
        if (input.extension() == ".dds") files.push_back(input);
        return files;
    }
    if (!fs::is_directory(input)) return files;

    for (const auto& entry : fs::recursive_directory_iterator(input)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".dds") continue;
        if (entry.path().filename().string().find("_restored") != std::string::npos) continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 7) {
        std::cout
            << "Usage: " << argv[0]
            << " <input.dds|folder> [raw_csv=overhead_raw.csv]"
               " [legacy_level=7] [runs=5] [sample=20]\n";
        return 1;
    }

    fs::path input = fs::absolute(argv[1]);
    fs::path rawCsv = argc >= 3 ? fs::absolute(argv[2])
                                : fs::absolute("overhead_raw.csv");
    int level = 7;
    int runs = 5;
    int samplePercent = 20;
    try {
        if (argc >= 4) level = std::stoi(argv[3]);
        if (argc >= 5) runs = std::stoi(argv[4]);
        if (argc >= 7) {
            // Backward compatibility with the old decoder.exe argument.
            samplePercent = std::stoi(argv[6]);
        } else if (argc >= 6) {
            samplePercent = std::stoi(argv[5]);
        }
    } catch (...) {
        std::cerr << "legacy_level, runs, and sample must be integers.\n";
        return 1;
    }
    if (!fs::exists(input)) {
        std::cerr << "Input does not exist: " << input.string() << '\n';
        return 1;
    }
    if (level != 7 && level != 8 && level != 9) {
        std::cerr << "pigz level must be 7, 8, or 9.\n";
        return 1;
    }
    if (runs < 1) {
        std::cerr << "runs must be at least 1.\n";
        return 1;
    }
    if (samplePercent != 100 &&
        samplePercent != 20 &&
        samplePercent != 10) {
        std::cerr << "sample must be 100, 20, or 10.\n";
        return 1;
    }
    std::vector<fs::path> files = CollectDdsFiles(input);
    if (files.empty()) {
        std::cerr << "No DDS files found.\n";
        return 1;
    }

    error_code ec;
    fs::create_directories(rawCsv.parent_path(), ec);
    if (ec) {
        std::cerr << "Cannot create output directory.\n";
        return 1;
    }

    fs::path summaryCsv = rawCsv.parent_path() /
                          (rawCsv.stem().string() + "_summary.csv");
    auto uniqueValue = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    fs::path workRoot = rawCsv.parent_path() /
                        ("overhead_work_" + std::to_string(uniqueValue));
    fs::create_directories(workRoot, ec);
    if (ec) return 1;

    std::ofstream raw(rawCsv);
    std::ofstream summary(summaryCsv);
    if (!raw || !summary) {
        std::cerr << "Cannot open CSV output.\n";
        fs::remove_all(workRoot, ec);
        return 1;
    }

    raw << "file,run,legacy_level,archive_codec,sample_percent,simulation_engine,"
           "original_bytes,prepared_bytes,compressed_bytes,"
           "scan_method,preprocess_ms,secondary_compress_ms,total_encode_ms,"
           "decode_core_ms,decode_write_ms,total_decode_ms,verified\n";
    summary << "file,runs,legacy_level,archive_codec,sample_percent,simulation_engine,"
               "original_bytes,compressed_bytes,ratio,"
               "preprocess_avg_ms,secondary_compress_avg_ms,total_encode_avg_ms,"
               "decode_core_avg_ms,decode_write_avg_ms,total_decode_avg_ms,"
               "all_verified\n";

    archiveMode = "lz4";
    compressionLevel = level;
    std::vector<FileAverage> averages;
    bool allVerified = true;

    for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        const fs::path& source = files[fileIndex];
        fs::path relative = fs::is_directory(input)
                                ? fs::relative(source, input, ec)
                                : source.filename();
        if (ec) relative = source.filename();

        FileAverage average;
        average.relativePath = relative.generic_string();
        average.originalBytes = fs::file_size(source);
        bool fileVerified = true;

        std::cout << '[' << (fileIndex + 1) << '/' << files.size() << "] "
                  << average.relativePath << '\n';

        for (int run = 1; run <= runs; ++run) {
            fs::path runDir = workRoot /
                              ("file_" + std::to_string(fileIndex) +
                               "_run_" + std::to_string(run));
            fs::path decodeDir = runDir / "decoded";
            fs::create_directories(decodeDir, ec);
            if (ec) return 2;

            fs::path archive = runDir /
                               (source.filename().string() + ".packed.lz4");
            int bestMethod = 0;
            vector<uint8_t> prepared;
            vector<uint8_t> packed;

            auto preprocessBegin = Clock::now();
            bool preparedOk = BuildOurPreprocessedData(
                source, prepared, bestMethod, -1, nullptr, 0x5U,
                samplePercent);
            auto preprocessEnd = Clock::now();
            if (!preparedOk) {
                std::cerr << "Preprocessing failed: " << source.string() << '\n';
                return 2;
            }

            auto secondaryBegin = Clock::now();
            bool compressedOk =
                PackedLz4::Compress(prepared, packed) &&
                WriteWholeFile(archive, packed.data(), packed.size());
            auto secondaryEnd = Clock::now();
            if (!compressedOk) {
                std::cerr << "LZ4 compression failed: " << source.string() << '\n';
                return 2;
            }

            fs::path restored = decodeDir / source.filename();
            vector<uint8_t> decodedPrepared;
            vector<uint8_t> restoredDds;
            auto decodeCoreBegin = Clock::now();
            bool decoded = PackedLz4::Decompress(packed, decodedPrepared) &&
                           PreprocessedRestore::ToDds(
                               decodedPrepared, restoredDds);
            auto decodeCoreEnd = Clock::now();
            auto decodeWriteBegin = Clock::now();
            bool wrote = decoded &&
                         WriteWholeFile(restored, restoredDds.data(),
                                        restoredDds.size());
            auto decodeWriteEnd = Clock::now();

            bool verified = wrote && FilesEqual(source, restored);
            fileVerified = fileVerified && verified;
            allVerified = allVerified && verified;

            double preprocessMs = ElapsedMs(preprocessBegin, preprocessEnd);
            double secondaryMs = ElapsedMs(secondaryBegin, secondaryEnd);
            double totalEncodeMs = preprocessMs + secondaryMs;
            double decodeCoreMs =
                ElapsedMs(decodeCoreBegin, decodeCoreEnd);
            double decodeWriteMs =
                ElapsedMs(decodeWriteBegin, decodeWriteEnd);
            double decodeMs = decodeCoreMs + decodeWriteMs;
            average.compressedBytes = fs::file_size(archive);
            average.preprocessMs += preprocessMs;
            average.secondaryMs += secondaryMs;
            average.totalEncodeMs += totalEncodeMs;
            average.decodeCoreMs += decodeCoreMs;
            average.decodeWriteMs += decodeWriteMs;
            average.totalDecodeMs += decodeMs;

            raw << Csv(average.relativePath) << ',' << run << ',' << level
                << ",LZ4-default," << samplePercent << ",LZ4-default,"
                << average.originalBytes << ',' << prepared.size() << ','
                << average.compressedBytes << ','
                << OrderMethodName(bestMethod) << ','
                << std::fixed << std::setprecision(3)
                << preprocessMs << ',' << secondaryMs << ',' << totalEncodeMs << ','
                << decodeCoreMs << ',' << decodeWriteMs << ',' << decodeMs
                << ',' << (verified ? "true" : "false") << '\n';
            raw.flush();

            fs::remove_all(runDir, ec);
        }

        average.preprocessMs /= runs;
        average.secondaryMs /= runs;
        average.totalEncodeMs /= runs;
        average.decodeCoreMs /= runs;
        average.decodeWriteMs /= runs;
        average.totalDecodeMs /= runs;
        averages.push_back(average);

        summary << Csv(average.relativePath) << ',' << runs << ',' << level
                << ",LZ4-default," << samplePercent << ",LZ4-default,"
                << average.originalBytes << ',' << average.compressedBytes << ','
                << std::fixed << std::setprecision(6)
                << static_cast<double>(average.originalBytes) / average.compressedBytes << ','
                << std::setprecision(3)
                << average.preprocessMs << ',' << average.secondaryMs << ','
                << average.totalEncodeMs << ',' << average.decodeCoreMs << ','
                << average.decodeWriteMs << ',' << average.totalDecodeMs << ','
                << (fileVerified ? "true" : "false") << '\n';
        summary.flush();
    }

    double preprocessTotal = 0.0;
    double secondaryTotal = 0.0;
    double encodeTotal = 0.0;
    double decodeCoreTotal = 0.0;
    double decodeWriteTotal = 0.0;
    double decodeTotal = 0.0;
    std::uintmax_t originalTotal = 0;
    std::uintmax_t compressedTotal = 0;
    for (const auto& average : averages) {
        preprocessTotal += average.preprocessMs;
        secondaryTotal += average.secondaryMs;
        encodeTotal += average.totalEncodeMs;
        decodeCoreTotal += average.decodeCoreMs;
        decodeWriteTotal += average.decodeWriteMs;
        decodeTotal += average.totalDecodeMs;
        originalTotal += average.originalBytes;
        compressedTotal += average.compressedBytes;
    }

    fs::remove_all(workRoot, ec);
    std::cout << std::fixed << std::setprecision(3)
              << "Files: " << averages.size() << '\n'
              << "Runs per file: " << runs << '\n'
              << "Archive codec: LZ4-default\n"
              << "Sample: " << samplePercent << "%\n"
              << "Simulation engine: LZ4-default\n"
              << "Preprocess total average: " << preprocessTotal << " ms\n"
              << "Secondary compression total average: " << secondaryTotal << " ms\n"
              << "Total encode average: " << encodeTotal << " ms\n"
              << "Decode core total average: " << decodeCoreTotal << " ms\n"
              << "Decode write total average: " << decodeWriteTotal << " ms\n"
              << "Total decode average: " << decodeTotal << " ms\n"
              << "Overall ratio: "
              << static_cast<double>(originalTotal) / compressedTotal << '\n'
              << "Verified: " << (allVerified ? "true" : "false") << '\n'
              << "Raw CSV: " << rawCsv.string() << '\n'
              << "Summary CSV: " << summaryCsv.string() << '\n';
    return allVerified ? 0 : 3;
}

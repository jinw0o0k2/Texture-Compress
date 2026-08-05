#define main texture_compress_encoder_main
#include "encoder.cpp"
#undef main

#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>

#include "PreprocessedRestore.hpp"

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::array<int, 3> kSamples = {100, 20, 10};

struct Aggregate {
    std::uintmax_t originalBytes = 0;
    std::uintmax_t compressedBytes = 0;
    double preprocessMs = 0.0;
    double secondaryMs = 0.0;
    double encodeMs = 0.0;
    double decodeCoreMs = 0.0;
    double decodeWriteMs = 0.0;
    double decodeMs = 0.0;
    int scanlineSelections = 0;
    int zOrderSelections = 0;
    bool verified = true;
};

double Ms(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::string Csv(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool FilesEqual(const fs::path& left, const fs::path& right) {
    error_code ec;
    if (!fs::exists(left) || !fs::exists(right) ||
        fs::file_size(left, ec) != fs::file_size(right, ec) || ec) {
        return false;
    }
    std::ifstream a(left, std::ios::binary);
    std::ifstream b(right, std::ios::binary);
    std::vector<char> aBuffer(1 << 20);
    std::vector<char> bBuffer(1 << 20);
    while (a && b) {
        a.read(aBuffer.data(), static_cast<std::streamsize>(aBuffer.size()));
        b.read(bBuffer.data(), static_cast<std::streamsize>(bBuffer.size()));
        if (a.gcount() != b.gcount() ||
            !std::equal(aBuffer.begin(), aBuffer.begin() + a.gcount(),
                        bBuffer.begin())) {
            return false;
        }
    }
    return true;
}

std::vector<fs::path> CollectDds(const fs::path& input) {
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
    if (argc < 2 || argc > 6) {
        std::cout << "Usage: " << argv[0]
                  << " <input.dds|folder> [output_dir=sampling_comparison]"
                     " [legacy_level=7] [runs=5] [ignored_decoder_arg]\n";
        return 1;
    }

    fs::path input = fs::absolute(argv[1]);
    fs::path output = argc >= 3 ? fs::absolute(argv[2])
                                : fs::absolute("sampling_comparison");
    int level = 7;
    int runs = 5;
    try {
        if (argc >= 4) level = std::stoi(argv[3]);
        if (argc >= 5) runs = std::stoi(argv[4]);
    } catch (...) {
        std::cerr << "legacy level and runs must be integers.\n";
        return 1;
    }
    if (!fs::exists(input) ||
        (level != 7 && level != 8 && level != 9) || runs < 1) {
        std::cerr << "Invalid input, level, or run count.\n";
        return 1;
    }

    std::vector<fs::path> files = CollectDds(input);
    if (files.empty()) return 1;

    error_code ec;
    fs::create_directories(output, ec);
    if (ec) return 1;
    fs::path work = output / "work";
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    if (ec) return 1;

    std::ofstream raw(output / "sampling_raw.csv");
    std::ofstream perFile(output / "sampling_per_file.csv");
    std::ofstream total(output / "sampling_total.csv");
    if (!raw || !perFile || !total) return 1;

    raw << "file,sample_percent,simulation_engine,archive_codec,run,selected_method,original_bytes,"
           "compressed_bytes,compressed_percent,preprocess_ms,secondary_ms,"
           "total_encode_ms,decode_core_ms,decode_write_ms,total_decode_ms,"
           "verified\n";
    perFile << "file,sample_percent,simulation_engine,archive_codec,runs,selected_method,original_bytes,"
               "compressed_bytes,compressed_percent,preprocess_avg_ms,"
               "secondary_avg_ms,total_encode_avg_ms,decode_core_avg_ms,"
               "decode_write_avg_ms,total_decode_avg_ms,"
               "matches_100_percent,all_verified\n";
    total << "sample_percent,simulation_engine,archive_codec,files,runs,scanline_selections,zorder_selections,"
             "matches_100_percent,original_bytes,compressed_bytes,"
             "compressed_percent,ratio,preprocess_total_avg_ms,"
             "secondary_total_avg_ms,total_encode_avg_ms,decode_core_total_avg_ms,"
             "decode_write_total_avg_ms,total_decode_avg_ms,all_verified\n";

    archiveMode = "lz4";
    compressionLevel = level;
    std::map<int, Aggregate> totals;
    std::map<int, int> matches100;

    for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
        const fs::path& source = files[fileIndex];
        fs::path relative = fs::is_directory(input)
                                ? fs::relative(source, input, ec)
                                : source.filename();
        if (ec) relative = source.filename();
        std::cout << '[' << (fileIndex + 1) << '/' << files.size() << "] "
                  << relative.generic_string() << '\n';

        std::array<Aggregate, 3> fileValues;
        std::array<int, 3> selectedMethods = {-1, -1, -1};
        for (auto& value : fileValues) {
            value.originalBytes = fs::file_size(source);
        }

        for (int run = 1; run <= runs; ++run) {
            int first = static_cast<int>(
                (fileIndex + static_cast<size_t>(run)) % kSamples.size());
            for (int order = 0; order < static_cast<int>(kSamples.size());
                 ++order) {
                int sampleIndex =
                    (first + order) % static_cast<int>(kSamples.size());
                int samplePercent = kSamples[sampleIndex];
                Aggregate& value = fileValues[sampleIndex];

                fs::path runDir =
                    work / ("p" + std::to_string(samplePercent) +
                            "_f" + std::to_string(fileIndex) +
                            "_r" + std::to_string(run));
                fs::path decodeDir = runDir / "decoded";
                fs::create_directories(decodeDir, ec);
                if (ec) return 2;
                fs::path archive =
                    runDir / (source.filename().string() + ".packed.lz4");
                vector<uint8_t> prepared;
                vector<uint8_t> packed;

                int selectedMethod = -1;
                auto preprocessBegin = Clock::now();
                bool built = BuildOurPreprocessedData(
                    source, prepared, selectedMethod, -1, nullptr, 0x5U,
                    samplePercent);
                auto preprocessEnd = Clock::now();
                if (!built) return 2;
                if (selectedMethods[sampleIndex] < 0) {
                    selectedMethods[sampleIndex] = selectedMethod;
                }
                if (selectedMethods[sampleIndex] != selectedMethod) {
                    std::cerr << "Non-deterministic selection.\n";
                    return 2;
                }

                auto secondaryBegin = Clock::now();
                bool compressed =
                    PackedLz4::Compress(prepared, packed) &&
                    WriteWholeFile(archive, packed.data(), packed.size());
                auto secondaryEnd = Clock::now();
                if (!compressed) return 2;

                fs::path restored = decodeDir / source.filename();
                vector<uint8_t> decodedPrepared;
                vector<uint8_t> restoredDds;
                auto decodeCoreBegin = Clock::now();
                bool decoded =
                    PackedLz4::Decompress(packed, decodedPrepared) &&
                    PreprocessedRestore::ToDds(
                        decodedPrepared, restoredDds);
                auto decodeCoreEnd = Clock::now();
                auto decodeWriteBegin = Clock::now();
                bool wrote = decoded &&
                             WriteWholeFile(restored, restoredDds.data(),
                                            restoredDds.size());
                auto decodeWriteEnd = Clock::now();

                bool verified = wrote && FilesEqual(source, restored);
                value.verified = value.verified && verified;

                double preprocessMs =
                    Ms(preprocessBegin, preprocessEnd);
                double secondaryMs =
                    Ms(secondaryBegin, secondaryEnd);
                double encodeMs = preprocessMs + secondaryMs;
                double decodeCoreMs = Ms(decodeCoreBegin, decodeCoreEnd);
                double decodeWriteMs = Ms(decodeWriteBegin, decodeWriteEnd);
                double decodeMs = decodeCoreMs + decodeWriteMs;
                value.compressedBytes = fs::file_size(archive);
                value.preprocessMs += preprocessMs;
                value.secondaryMs += secondaryMs;
                value.encodeMs += encodeMs;
                value.decodeCoreMs += decodeCoreMs;
                value.decodeWriteMs += decodeWriteMs;
                value.decodeMs += decodeMs;

                raw << Csv(relative.generic_string()) << ',' << samplePercent
                    << ",LZ4-default,LZ4-default," << run << ',' << OrderMethodName(selectedMethod)
                    << ',' << value.originalBytes << ','
                    << value.compressedBytes << ','
                    << std::fixed << std::setprecision(6)
                    << (100.0 * value.compressedBytes /
                        value.originalBytes) << ','
                    << std::setprecision(3) << preprocessMs << ','
                    << secondaryMs << ',' << encodeMs << ',' << decodeCoreMs
                    << ',' << decodeWriteMs << ',' << decodeMs
                    << ',' << (verified ? "true" : "false") << '\n';
                raw.flush();
                fs::remove_all(runDir, ec);
            }
        }

        for (int sampleIndex = 0;
             sampleIndex < static_cast<int>(kSamples.size());
             ++sampleIndex) {
            int samplePercent = kSamples[sampleIndex];
            Aggregate& value = fileValues[sampleIndex];
            value.preprocessMs /= runs;
            value.secondaryMs /= runs;
            value.encodeMs /= runs;
            value.decodeCoreMs /= runs;
            value.decodeWriteMs /= runs;
            value.decodeMs /= runs;
            value.scanlineSelections =
                selectedMethods[sampleIndex] == 0 ? 1 : 0;
            value.zOrderSelections =
                selectedMethods[sampleIndex] == 2 ? 1 : 0;
            bool matches =
                selectedMethods[sampleIndex] == selectedMethods[0];
            if (matches) ++matches100[samplePercent];

            perFile << Csv(relative.generic_string()) << ',' << samplePercent
                    << ",LZ4-default,LZ4-default," << runs << ','
                    << OrderMethodName(selectedMethods[sampleIndex]) << ','
                    << value.originalBytes << ',' << value.compressedBytes
                    << ',' << std::fixed << std::setprecision(6)
                    << (100.0 * value.compressedBytes /
                        value.originalBytes) << ','
                    << std::setprecision(3) << value.preprocessMs << ','
                    << value.secondaryMs << ',' << value.encodeMs << ','
                    << value.decodeCoreMs << ',' << value.decodeWriteMs << ','
                    << value.decodeMs << ',' << (matches ? "true" : "false")
                    << ',' << (value.verified ? "true" : "false") << '\n';

            Aggregate& aggregate = totals[samplePercent];
            aggregate.originalBytes += value.originalBytes;
            aggregate.compressedBytes += value.compressedBytes;
            aggregate.preprocessMs += value.preprocessMs;
            aggregate.secondaryMs += value.secondaryMs;
            aggregate.encodeMs += value.encodeMs;
            aggregate.decodeCoreMs += value.decodeCoreMs;
            aggregate.decodeWriteMs += value.decodeWriteMs;
            aggregate.decodeMs += value.decodeMs;
            aggregate.scanlineSelections += value.scanlineSelections;
            aggregate.zOrderSelections += value.zOrderSelections;
            aggregate.verified = aggregate.verified && value.verified;
        }
        perFile.flush();
    }

    for (int samplePercent : kSamples) {
        const Aggregate& value = totals.at(samplePercent);
        total << samplePercent << ",LZ4-default,LZ4-default,"
              << files.size() << ',' << runs << ','
              << value.scanlineSelections << ',' << value.zOrderSelections
              << ',' << matches100[samplePercent] << ','
              << value.originalBytes << ',' << value.compressedBytes << ','
              << std::fixed << std::setprecision(6)
              << (100.0 * value.compressedBytes / value.originalBytes) << ','
              << (static_cast<double>(value.originalBytes) /
                  value.compressedBytes) << ','
              << std::setprecision(3) << value.preprocessMs << ','
              << value.secondaryMs << ',' << value.encodeMs << ','
              << value.decodeCoreMs << ',' << value.decodeWriteMs << ','
              << value.decodeMs << ','
              << (value.verified ? "true" : "false") << '\n';
    }

    fs::remove_all(work, ec);
    bool verified = true;
    for (int samplePercent : kSamples) {
        verified = verified && totals[samplePercent].verified;
    }
    std::cout << "Simulation engine: LZ4-default\n"
              << "Archive codec: LZ4-default (in-process)\n"
              << "Completed sampling comparison. Verified: "
              << (verified ? "true" : "false") << '\n';
    return verified ? 0 : 3;
}

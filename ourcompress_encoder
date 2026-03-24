#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <filesystem>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <fstream>
#include <future>      // 멀티스레딩
#include <execution>   // 병렬 정렬
#include <memory>      // 스마트 포인터 (Zero-fill 방지)
#include "ScanAlgorithms.hpp" 
#include "miniz.h"     // In-Memory 압축 라이브러리

using namespace std;
namespace fs = std::filesystem;

// ==========================================
// [구조체 정의]
// ==========================================
struct BlockData {
    uint64_t sortKey; 
    uint32_t originalLinearIdx; 
    uint16_t c0, c1;
    uint32_t c_idx;
    uint8_t a0, a1;
    uint64_t a_idx; 

    bool operator<(const BlockData& other) const {
        return sortKey < other.sortKey;
    }
};

void ApplyIndexReordering(vector<BlockData>& blocks, uint32_t width, uint32_t height, bool isBC3) {
    return;
}

void RemoveFile(const string& filename) { 
    if (fs::exists(filename)) {
        error_code ec;
        fs::remove(filename, ec); 
    }
}

long GetFileSize(const string& filename) {
    if (fs::exists(filename)) {
        error_code ec;
        return fs::file_size(filename, ec);
    }
    return 0;
}

// ==========================================
// [코어 로직] 인코더 클래스
// ==========================================
class DDSCompressor {
private:
    string inputPath;
    string exe7z = "\"C:\\Program Files\\7-Zip\\7z.exe\"";
    
public:
    DDSCompressor(string path) : inputPath(path) {}

    pair<double, long> Process(bool silent = false) {
        auto start_time = chrono::high_resolution_clock::now();

        FILE* fp = fopen(inputPath.c_str(), "rb");
        if (!fp) return {0.0, 0};

        uint8_t header[128];
        fread(header, 1, 128, fp);
        uint32_t height = *(uint32_t*)(header + 12);
        uint32_t width = *(uint32_t*)(header + 16);
        uint32_t fourCC = *(uint32_t*)(header + 84);

        bool isBC3 = false; bool isBC4 = false; int blockSize = 8;
        if (fourCC == 0x35545844) { isBC3 = true; blockSize = 16; } 
        else if (fourCC == 0x31495441 || fourCC == 0x55344342) { isBC4 = true; blockSize = 8; }

        fseek(fp, 0, SEEK_END);
        size_t fileSize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        vector<uint8_t> originBuffer(fileSize);
        fread(originBuffer.data(), 1, fileSize, fp);
        fclose(fp);

        uint32_t blocksW = (width + 3) / 4;
        uint32_t blocksH = (height + 3) / 4;
        size_t blockCount = (fileSize - 128) / blockSize;
        uint8_t* dPtr = originBuffer.data() + 128;

        vector<BlockData> blocks(blockCount);

        for (size_t linearIdx = 0; linearIdx < blockCount; ++linearIdx) {
            BlockData b; 
            b.originalLinearIdx = linearIdx;
            size_t offset = linearIdx * blockSize;

            if (isBC3) {
                b.a0 = dPtr[offset + 0]; b.a1 = dPtr[offset + 1];
                b.a_idx = 0; memcpy(&b.a_idx, dPtr + offset + 2, 6);
                memcpy(&b.c0, dPtr + offset + 8, 2); memcpy(&b.c1, dPtr + offset + 10, 2); memcpy(&b.c_idx, dPtr + offset + 12, 4);
            } else if (isBC4) {
                b.a0 = dPtr[offset + 0]; b.a1 = dPtr[offset + 1];
                b.a_idx = 0; memcpy(&b.a_idx, dPtr + offset + 2, 6);
                b.c0 = 0; b.c1 = 0; b.c_idx = 0;
            } else {
                b.a0 = 0; b.a1 = 0; b.a_idx = 0; 
                memcpy(&b.c0, dPtr + offset, 2); memcpy(&b.c1, dPtr + offset + 2, 2); memcpy(&b.c_idx, dPtr + offset + 4, 4);
            }
            blocks[linearIdx] = b;
        }

        size_t bytesPerBlock = isBC4 ? 8 : (isBC3 ? 16 : 8); 
        size_t totalBufferSize = blockCount * bytesPerBlock;

        // 최적화: In-Memory + 멀티스레딩 + 번호표 정렬 + Zero-fill 제거
        auto CalculateSizeInMemory = [&](int method) -> long {
            vector<uint32_t> idxMap(blockCount);
            iota(idxMap.begin(), idxMap.end(), 0);

            vector<uint64_t> keys(blockCount);
            for(size_t i=0; i<blockCount; ++i) {
                uint32_t y = i / blocksW; uint32_t x = i % blocksW;
                keys[i] = (method == 1) ? ScanAlgorithms::getHilbertIndex(blocksW, x, y) : ScanAlgorithms::getScanlineIndex(x, y, blocksW);
            }

            sort(std::execution::par, idxMap.begin(), idxMap.end(), [&](uint32_t a, uint32_t b) {
                return keys[a] < keys[b];
            });

            unique_ptr<uint8_t[]> simBuf(new uint8_t[totalBufferSize]);
            size_t offset = 0;

            for(size_t i=0; i<blockCount; ++i) {
                const auto& b = blocks[idxMap[i]]; 
                if(isBC3 || isBC4) { 
                    simBuf[offset++] = b.a0; simBuf[offset++] = b.a1;
                    memcpy(&simBuf[offset], &b.a_idx, 6); offset += 6;
                }
                if(!isBC4) { 
                    memcpy(&simBuf[offset], &b.c0, 2); offset += 2;
                    memcpy(&simBuf[offset], &b.c1, 2); offset += 2;
                    memcpy(&simBuf[offset], &b.c_idx, 4); offset += 4;
                }
            }

            unsigned long compressedLen = mz_compressBound((unsigned long)totalBufferSize);
            unique_ptr<uint8_t[]> compressedBuf(new uint8_t[compressedLen]);

            int status = mz_compress(compressedBuf.get(), &compressedLen, simBuf.get(), (unsigned long)totalBufferSize);
            
            if (status != MZ_OK) return 0; 
            return (long)compressedLen; 
        };

        // 스레드 2개로  동시 실행
        auto futureScan = std::async(std::launch::async, CalculateSizeInMemory, 0);
        auto futureHilb = std::async(std::launch::async, CalculateSizeInMemory, 1);

        long sizeScan = futureScan.get(); 
        long sizeHilb = futureHilb.get(); 
        int bestMethod = (sizeHilb < sizeScan) ? 1 : 0;

        // 최종 출력을 위한  정렬 세팅
        for(size_t i=0; i<blocks.size(); ++i) {
            uint32_t y = blocks[i].originalLinearIdx / blocksW; 
            uint32_t x = blocks[i].originalLinearIdx % blocksW;
            blocks[i].sortKey = (bestMethod == 1) ? ScanAlgorithms::getHilbertIndex(blocksW, x, y) : ScanAlgorithms::getScanlineIndex(x, y, blocksW);
        }
        
        sort(std::execution::par, blocks.begin(), blocks.end());
        ApplyIndexReordering(blocks, width, height, isBC3);

        //  최종 파일 쓰기 버퍼 
        unique_ptr<uint8_t[]> finalBuf(new uint8_t[128 + 1 + totalBufferSize]);
        size_t finalOffset = 0;
        
        memcpy(&finalBuf[finalOffset], header, 128); finalOffset += 128;
        uint8_t methodFlag = bestMethod;
        memcpy(&finalBuf[finalOffset], &methodFlag, 1); finalOffset += 1;

        if (isBC3 || isBC4) {
            for(const auto& b : blocks) { finalBuf[finalOffset++] = b.a0; finalBuf[finalOffset++] = b.a1; }
            for(const auto& b : blocks) { memcpy(&finalBuf[finalOffset], &b.a_idx, 6); finalOffset += 6; }
        }
        if (!isBC4) {
            for(const auto& b : blocks) { 
                memcpy(&finalBuf[finalOffset], &b.c0, 2); finalOffset += 2; 
                memcpy(&finalBuf[finalOffset], &b.c1, 2); finalOffset += 2; // ⭐ 빼먹었던 c1 무사히 귀환 완료
            }
            for(const auto& b : blocks) { memcpy(&finalBuf[finalOffset], &b.c_idx, 4); finalOffset += 4; }
        }

        string tempBinName = "temp_pack.bin";
        FILE* fOut = fopen(tempBinName.c_str(), "wb");
        if(fOut) {
            fwrite(finalBuf.get(), 1, finalOffset, fOut);
            fclose(fOut);
        }

        string outputDir = "Compressed_File";
        if (!fs::exists(outputDir)) fs::create_directory(outputDir);

        string fileName = fs::path(inputPath).filename().string();
        string outputPath = outputDir + "/" + fileName + ".packed.zip";
        
        RemoveFile(outputPath);

        string cmd = "\"" + exe7z + " a -tzip -mx=9 -bd \"" + outputPath + "\" " + tempBinName + " > NUL 2>&1\"";
        int ret = system(cmd.c_str());
        
        long finalSize = (ret == 0) ? GetFileSize(outputPath) : 0;

        auto end_time = chrono::high_resolution_clock::now();
        chrono::duration<double> total_time = end_time - start_time;

        RemoveFile(tempBinName);
        return {total_time.count(), finalSize};
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: " << argv[0] << " [folder_path]" << endl;
        return 1;
    }

    string folderPath = argv[1];
    if (!fs::exists(folderPath)) return 1;

    ofstream csvFile("benchmark_results_mf3.csv");
    if (!csvFile.is_open()) return 1;

    csvFile << "Filename,Size_Bytes,Run1(s),Run2(s),Run3(s),Run4(s),Run5(s),Run6(s),Run7(s),Run8(s),Run9(s),Run10(s),Average(s),StdDev(s)\n";

    cout << "=== Starting 10-Iteration ZIP Compression Benchmark (Extreme Optimized) ===" << endl;
    
    for (const auto& entry : fs::recursive_directory_iterator(folderPath)) {
        if (entry.path().extension() == ".dds") {
            string name = entry.path().filename().string();
            if (name.find("_restored") != string::npos) continue;

            cout << "Benchmarking: " << name << " (10 runs)... " << flush;
            
            try {
                DDSCompressor compressor(entry.path().string());
                vector<double> times;
                long finalSize = 0;
                
                for (int i = 0; i < 10; ++i) {
                    auto result = compressor.Process(true);
                    if (result.first > 0.0) {
                        times.push_back(result.first);
                        finalSize = result.second;
                    }
                }

                if (times.size() == 10) {
                    double sum = accumulate(times.begin(), times.end(), 0.0);
                    double mean = sum / 10.0;
                    double sq_sum = inner_product(times.begin(), times.end(), times.begin(), 0.0);
                    double stdev = sqrt((sq_sum / 10.0) - (mean * mean));

                    csvFile << name << "," << finalSize << ",";
                    for(double t : times) csvFile << t << ",";
                    csvFile << mean << "," << stdev << "\n";
                    csvFile.flush();

                    cout << "Done! Avg: " << mean << " s, StdDev: " << stdev << " s" << endl;
                } else {
                    cout << "Failed." << endl;
                }
            } catch (const exception& e) {
                cout << "Error occurred: " << e.what() << endl;
            }
        }
    }

    csvFile.close();
    cout << "All jobs finished. Results saved." << endl;
    return 0;
}

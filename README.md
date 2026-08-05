# Texture Compress

DDS의 BC 압축 블록을 Scanline 또는 Z-order로 배치하고 채널별로 분리한 뒤,
pigz ZIP 또는 7Z로 2차 압축하는 Windows용 C++ 인코더/디코더입니다.

압축파일 내부에는 일반 DDS가 아니라 전처리된 BIN 데이터가 들어 있으므로
원본 DDS 복원에는 이 저장소의 `decoder.exe`가 필요합니다.

## 핵심 정책

자동 선택 모드는 다음과 같이 동작합니다.

1. DDS 헤더와 BC 블록을 읽습니다.
2. 다음 조건을 모두 만족하는지 검사합니다.
   - 블록 격자가 정사각형: `blocksW == blocksH`
   - 한 변의 블록 수가 2의 거듭제곱
   - 페이로드 블록 수가 `blocksW * blocksH`와 동일
3. 조건을 만족하지 않으면 후보 시뮬레이션 없이 Scanline으로 고정합니다.
4. 조건을 만족하면 전체 블록에서 균일하게 추출한 20% 샘플을
   LZ4 default로 압축하여 Scanline과 Z-order를 비교합니다.
5. 더 작은 후보를 선택하고 전체 블록을 해당 순서로 배치합니다.
6. 전처리된 BIN을 pigz ZIP 또는 7Z로 압축합니다.

기본 샘플 비율은 20%이며 CLI에서 10% 또는 100%로 변경할 수 있습니다.

## 정렬 제거

새 인코딩 경로에서는 `std::sort`를 사용하지 않습니다.

- Scanline: 원본 블록 순서를 그대로 사용합니다.
- Z-order: 정사각형 2ⁿ 블록 격자에서 Morton 키가 정확한 목적지 인덱스가
  되므로 `ordered[targetIdx] = source[linearIdx]`로 직접 배치합니다.
- 같은 해상도의 Z-order 인덱스는 LUT로 캐시합니다.
- 새 Z-order 디코딩도 같은 LUT로 원본 위치에 직접 복원합니다.

디코더에는 기존 Hilbert 압축파일을 복원하기 위한 레거시 호환 경로가
남아 있습니다. 새 인코더는 Hilbert 압축파일을 생성하지 않습니다.

## 파일

- `encoder.cpp`: DDS 파일 또는 폴더를 인코딩합니다.
- `decoder.cpp`: `.packed.zip` 또는 `.packed.7z`를 DDS로 복원합니다.
- `ScanAlgorithms.hpp`: Scanline, Hilbert 호환, Z-order 및 LUT 구현입니다.
- `overhead_benchmark.cpp`: 인코딩·디코딩 시간과 압축률을 기본 5회 측정합니다.
- `sampling_comparison_benchmark.cpp`: 100%·20%·10% 샘플링을 비교합니다.

## 요구 사항

- Windows
- C++17 컴파일러
- [LZ4 v1.10.0](https://github.com/lz4/lz4/releases/tag/v1.10.0)
  - Scanline/Z-order 후보 크기 비교에 사용합니다.
  - 최종 ZIP 압축기가 아니라 선택 시뮬레이션 엔진입니다.
- pigz
  - `pigz.exe`를 실행 파일 옆에 두거나 `PATH`에 추가합니다.
  - 또는 `PIGZ_EXE` 환경 변수에 전체 경로를 지정합니다.
- [7-Zip](https://www.7-zip.org/)
  - ZIP/7Z 해제와 선택적인 7Z 인코딩에 사용합니다.
  - 기본 경로는 `C:\Program Files\7-Zip\7z.exe`입니다.

## 빌드

저장소의 `CMakeLists.txt`가 공식 LZ4 v1.10.0을 자동으로 받아 정적
라이브러리로 연결합니다. 최초 구성에는 인터넷 연결과 Git이 필요합니다.

```bat
cmake -S . -B build
cmake --build build --config Release
```

생성 대상:

- `encoder`
- `decoder`
- `overhead_benchmark`
- `sampling_comparison_benchmark`

`overhead_benchmark.cpp`와 `sampling_comparison_benchmark.cpp`는 내부에서
`encoder.cpp`를 포함하므로 수동 빌드 시 `encoder.cpp`를 명령행에 다시
추가하지 마십시오.

## 인코더 사용법

```text
encoder.exe <input.dds|folder> [output_folder] [pigz|7z] [level=7] [auto|scanline|zorder] [sample=20|10|100]
```

기본 설정으로 폴더 전체 인코딩:

```bat
encoder.exe "C:\Textures" "C:\Encoded" pigz 7 auto 20
```

100% 후보 시뮬레이션:

```bat
encoder.exe "C:\Textures" "C:\Encoded" pigz 7 auto 100
```

Z-order 강제:

```bat
encoder.exe "C:\Textures\texture.dds" "C:\Encoded" pigz 7 zorder 20
```

Z-order 조건을 만족하지 않는 DDS에서는 `zorder`를 지정해도 안전을 위해
Scanline으로 처리합니다.

pigz 레벨은 `7`, `8`, `9`를 지원합니다. 기존 명령과의 호환을 위해
`zip`은 `pigz`의 별칭으로 처리됩니다.

## 디코더 사용법

```text
decoder.exe <archive|folder> [output_folder]
```

폴더 전체 복원:

```bat
decoder.exe "C:\Encoded" "C:\Restored"
```

단일 압축파일 복원:

```bat
decoder.exe "C:\Encoded\texture.dds.packed.zip" "C:\Restored"
```

복원된 DDS는 인코딩 전 원본과 바이트 단위로 동일해야 합니다.

## 오버헤드 측정

```text
overhead_benchmark.exe <input.dds|folder> [raw_csv=overhead_raw.csv] [pigz_level=7] [runs=5] [decoder.exe] [sample=20]
```

20% 샘플을 파일당 5회 측정:

```bat
overhead_benchmark.exe "C:\Textures" "C:\Results\raw.csv" 7 5 decoder.exe 20
```

측정 항목:

- `preprocess_ms`: 적격성 검사, 샘플 후보 비교, LUT 직접 배치 및 BIN 쓰기
- `secondary_compress_ms`: 전처리 BIN을 pigz로 압축하는 시간
- `total_encode_ms`: 전처리와 pigz 압축 시간의 합
- `total_decode_ms`: ZIP 해제부터 DDS 복원·쓰기까지의 시간
- `compressed_bytes`, `ratio`: 최종 크기와 압축 배율
- `scan_method`: 실제 선택된 Scanline 또는 Z-order
- `verified`: 복원 결과가 원본과 같은지 여부

원시 CSV와 파일별 평균 summary CSV를 생성합니다.

## 샘플링 비교

```text
sampling_comparison_benchmark.exe <input.dds|folder> [output_dir] [pigz_level=7] [runs=5] [decoder.exe]
```

100%·20%·10%를 파일과 회차마다 교차 실행하여 다음 파일을 생성합니다.

- `sampling_raw.csv`: 모든 실행의 원시값
- `sampling_per_file.csv`: 파일·샘플 비율별 평균과 100% 선택 일치 여부
- `sampling_total.csv`: 샘플 비율별 전체 합계

## 지원 범위 및 주의 사항

- BC1/DXT1, BC3/DXT5, BC4/ATI1 계열을 대상으로 작성했습니다.
- 128바이트 DDS 헤더를 전제로 합니다.
- DX10 확장 헤더는 지원하지 않습니다.
- 밉맵이나 배열·큐브 텍스처처럼 추가 서브리소스가 감지되면 Z-order를
  사용하지 않고 Scanline으로 처리합니다.
- 정확한 시간 측정 중에는 다른 CPU·메모리 집약 작업을 피하십시오.

## 종료 코드

- `0`: 모든 파일 처리 및 검증 성공
- `1`: 인자, 입력 경로 또는 필수 프로그램 오류
- `2`: 하나 이상의 파일 처리 실패
- `3`: 벤치마크 복원 검증 실패

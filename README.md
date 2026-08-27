# Texture Compress

DDS의 BC 압축 블록을 Scanline, Hilbert 또는 Z-order로 배치하고 채널별로
분리한 뒤, 인프로세스 LZ4, LZ4HC 또는 Zstandard로 2차 압축하는 C++
인코더/디코더입니다. 기존 실험과의 호환을 위해 외부 pigz ZIP 및 7Z
인코딩 경로도 선택적으로 남겨 두었습니다.

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
4. 조건을 만족하면 전체 블록에서 균일하게 추출한 20% 샘플을 사용해
   Scanline, Hilbert, Z-order를 비교합니다. 시뮬레이션 엔진은 기본 LZ4이며
   CLI에서 단일 스레드 Zstd level 1로 변경할 수 있습니다.
5. 더 작은 후보를 선택하고 전체 블록을 해당 순서로 배치합니다.
6. 전처리 데이터를 파일로 왕복시키지 않고 선택한 코덱으로 같은 프로세스
   안에서 압축합니다. 출력은 `.packed.lz4`, `.packed.lz4hc` 또는
   `.packed.zst`입니다.

기본 샘플 비율은 20%이며 CLI에서 10% 또는 100%로 변경할 수 있습니다.

## 정렬 제거

새 인코딩 경로에서는 `std::sort`를 사용하지 않습니다.

- Scanline: 원본 블록 순서를 그대로 사용합니다.
- Hilbert/Z-order: 정사각형 2ⁿ 블록 격자에서 lazy LUT를 사용해 정렬 없이
  원본 DDS 블록을 endpoint/index 분리 버퍼에 직접 기록합니다.
- 인코더는 target-to-linear LUT로 중간 전체 재배열 배열 없이 원본 블록을
  조회합니다.
- 디코더는 별도의 ordered/restored 블록 배열을 만들지 않고,
  linear-to-target LUT로 분리 버퍼에서 최종 DDS 위치에 직접 복원합니다.
- 같은 해상도의 정·역방향 LUT는 최초 한 번 생성한 뒤 캐시합니다.
- 이전 직사각형 Hilbert 압축파일은 레거시 정렬 경로로 복원할 수 있습니다.

## 파일

- `encoder.cpp`: DDS 파일 또는 폴더를 인코딩합니다.
- `decoder.cpp`: `.packed.lz4`, `.packed.lz4hc`, `.packed.zst`와 레거시
  `.packed.zip`, `.packed.7z`를 DDS로 복원합니다.
- `PackedLz4.hpp`: LZ4/LZ4HC 압축 포맷과 인프로세스 압축·해제를 구현합니다.
- `PackedZstd.hpp`: Zstd 압축 포맷과 MT 압축·인프로세스 해제를 구현합니다.
- `PreprocessedRestore.hpp`: planar 전처리 데이터를 원본 DDS로 복원합니다.
- `ScanAlgorithms.hpp`: Scanline, Hilbert, Z-order 및 lazy LUT를 구현합니다.
- `overhead_benchmark.cpp`: 인코딩·디코딩 시간과 압축률을 기본 5회 측정합니다.

## 요구 사항

- Windows
- C++17 컴파일러
- [LZ4 v1.10.0](https://github.com/lz4/lz4/releases/tag/v1.10.0)
  - 기본 후보 크기 비교와 선택적인 최종 압축·복원에 사용합니다.
  - CMake가 정적 라이브러리로 연결하므로 실행 시 `lz4.exe`가 필요하지 않습니다.
- [Zstandard v1.5.7](https://github.com/facebook/zstd/releases/tag/v1.5.7)
  - 선택적인 후보 크기 비교와 최종 압축·복원에 사용합니다.
  - 후보 비교 시에는 level 1 단일 스레드, 최종 압축 시에는 자동 MT를
    사용합니다.
  - 정적으로 연결하므로 실행 시 `zstd.exe`가 필요하지 않습니다.
  - 압축 시 CPU 논리 코어 수를 자동 감지하여 `ZSTD_c_nbWorkers`에 설정합니다.
    CSV의 `archive_codec`에는 예를 들어 `Zstd-1-MT16`처럼 기록됩니다.
- pigz (레거시 `pigz` 모드에서만 필요)
  - `pigz.exe`를 실행 파일 옆에 두거나 `PATH`에 추가합니다.
  - 또는 `PIGZ_EXE` 환경 변수에 전체 경로를 지정합니다.
- [7-Zip](https://www.7-zip.org/) (레거시 ZIP/7Z 파일에서만 필요)
  - 기존 ZIP/7Z 해제와 선택적인 7Z 인코딩에 사용합니다.
  - 기본 경로는 `C:\Program Files\7-Zip\7z.exe`입니다.

## 빌드

저장소의 `CMakeLists.txt`가 공식 LZ4 v1.10.0과 Zstandard v1.5.7을
자동으로 받아 정적 라이브러리로 연결합니다. 최초 구성에는 인터넷 연결과
Git이 필요합니다.

```bat
cmake -S . -B build
cmake --build build --config Release
```

생성 대상:

- `encoder`
- `decoder`
- `overhead_benchmark`

`overhead_benchmark.cpp`는 내부에서 `encoder.cpp`를 포함하므로 수동 빌드 시
`encoder.cpp`를 명령행에 다시 추가하지 마십시오.

## 인코더 사용법

```text
encoder.exe <input.dds|folder> [output_folder] [lz4|lz4hc|zstd|pigz|7z] [level] [auto|scanline|hilbert|zorder] [sample=20|10|100] [simulation=lz4|zstd]
```

기본 설정으로 폴더 전체 인코딩:

```bat
encoder.exe "C:\Textures" "C:\Encoded" lz4 0 auto 20
```

100% 후보 시뮬레이션:

```bat
encoder.exe "C:\Textures" "C:\Encoded" lz4 0 auto 100
```

Z-order 강제:

```bat
encoder.exe "C:\Textures\texture.dds" "C:\Encoded" lz4 0 zorder 20
```

Hilbert/Z-order 조건을 만족하지 않는 DDS에서는 해당 방식을 지정해도
안전을 위해 Scanline으로 처리합니다.

기본 모드는 `lz4`입니다. LZ4 default의 level 값은 사용하지 않습니다.
LZ4HC 기본 level은 `3`, Zstd 기본 level은 `1`이며 CLI에서 변경할 수
있습니다. LZ4HC는 `3`부터 `12`까지 지원합니다. pigz 레벨은 `7`, `8`,
`9`를 지원하며 기존 명령과의 호환을 위해 `zip`은 `pigz`의 별칭입니다.

```bat
encoder.exe "C:\Textures" "C:\EncodedLz4" lz4 0 auto 10
encoder.exe "C:\Textures" "C:\EncodedLz4hc" lz4hc 3 auto 10
encoder.exe "C:\Textures" "C:\EncodedZstd" zstd 1 auto 10 lz4
encoder.exe "C:\Textures" "C:\EncodedZstdSim" zstd 1 auto 10 zstd
```

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
decoder.exe "C:\Encoded\texture.dds.packed.lz4" "C:\Restored"
```

복원된 DDS는 인코딩 전 원본과 바이트 단위로 동일해야 합니다.

## 오버헤드 측정

```text
overhead_benchmark.exe <input.dds|folder> [raw_csv=overhead_raw.csv] [lz4|lz4hc|zstd] [level] [runs=5] [sample=20] [simulation=lz4|zstd]
```

10% 샘플을 파일당 5회 측정:

```bat
overhead_benchmark.exe "C:\Textures" "C:\Results\lz4.csv" lz4 0 5 10
overhead_benchmark.exe "C:\Textures" "C:\Results\lz4hc3.csv" lz4hc 3 5 10
overhead_benchmark.exe "C:\Textures" "C:\Results\zstd1.csv" zstd 1 5 10
overhead_benchmark.exe "C:\Textures" "C:\Results\zstd1_zstd_sim.csv" zstd 1 5 10 zstd
```

측정 항목:

- `preprocess_ms`: 적격성 검사, 샘플 후보 비교 및 LUT 기반 planar 버퍼 생성
- `secondary_compress_ms`: 전처리 데이터를 선택한 코덱으로 압축하고 쓰는 시간
- `total_encode_ms`: 전처리와 최종 압축·쓰기 시간의 합
- `decode_core_ms`: 별도 프로세스 없이 압축 해제와 DDS 메모리 복원 시간
- `decode_write_ms`: 복원된 DDS를 파일로 쓰는 시간
- `total_decode_ms`: `decode_core_ms`와 `decode_write_ms`의 합
- `compressed_bytes`, `ratio`: 최종 크기와 압축 배율
- `simulation_engine`: 후보 비교에 사용한 LZ4 또는 단일 스레드 Zstd
- `scan_method`: 실제 선택된 Scanline, Hilbert 또는 Z-order
- `verified`: 복원 결과가 원본과 같은지 여부

원시 CSV와 파일별 평균 summary CSV를 생성합니다.

## 실험 결과 관리

기존 결과는 `results/`에 유지합니다. 새 결과를 Git에 추가할 때는 날짜와
실험 내용을 함께 표시한 디렉터리에 저장합니다.

```text
results/YYYY-MM-DD_실험내용/
```

각 디렉터리에는 원시 CSV, summary CSV와 실험 조건을 적은 `README.md`를
함께 저장합니다. 예:

```text
results/2026-08-20_lz4-vs-zstd-simulation/
├─ simulation_lz4.csv
├─ simulation_zstd.csv
└─ README.md
```

## 지원 범위 및 주의 사항

- BC1/DXT1, BC3/DXT5, BC4/ATI1·BC4U를 지원합니다. 다른 FourCC는
  잘못된 BC1 데이터로 처리하지 않고 명시적으로 거부합니다.
- 128바이트 DDS 헤더를 전제로 합니다.
- DX10 확장 헤더는 지원하지 않습니다.
- 밉맵이나 배열·큐브 텍스처처럼 추가 서브리소스가 감지되면 Hilbert와
  Z-order를 사용하지 않고 Scanline으로 처리합니다.
- 정확한 시간 측정 중에는 다른 CPU·메모리 집약 작업을 피하십시오.

## 종료 코드

- `0`: 모든 파일 처리 및 검증 성공
- `1`: 인자, 입력 경로 또는 필수 프로그램 오류
- `2`: 하나 이상의 파일 처리 실패
- `3`: 벤치마크 복원 검증 실패

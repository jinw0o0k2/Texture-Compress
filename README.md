# Texture Compress

DDS의 BC 압축 블록을 압축 친화적인 순서와 구조로 재배치한 뒤 pigz ZIP 또는 7Z로 저장하고, 이를 다시 원본 DDS 바이트 배열로 복원하는 Windows용 C++ 인코더/디코더입니다.

생성된 압축파일에는 일반 DDS가 아니라 전처리된 사용자 정의 BIN 데이터가 들어 있습니다. 원본 DDS로 복원할 때는 반드시 이 저장소의 `decoder.exe`를 사용해야 합니다.

## 파일

- `encoder.cpp`: DDS 파일 또는 폴더를 `.packed.zip` 또는 `.packed.7z`로 인코딩합니다.
- `decoder.cpp`: 인코더가 만든 압축파일을 원본 DDS로 복원합니다.
- `overhead_benchmark.cpp`: 우리 방식의 전처리, 2차 압축, 전체 인코딩 및 전체 디코딩 오버헤드를 측정합니다.
- `ScanAlgorithms.hpp`: Scanline 및 직사각형 텍스처용 Hilbert 인덱스를 제공합니다.

## 처리 방식

인코더는 각 DDS에 대해 다음 작업을 수행합니다.

1. 128바이트 DDS 헤더와 BC 블록을 읽습니다.
2. Scanline과 Hilbert 순서의 예상 압축 크기를 `miniz`로 동시에 계산합니다.
3. 더 작은 결과를 만드는 순서를 선택합니다.
4. BC 블록의 엔드포인트와 인덱스 데이터를 압축 친화적으로 분리·재배열합니다.
5. 전처리된 BIN을 pigz Deflate ZIP 또는 7-Zip LZMA2 기반 7Z로 압축합니다.

pigz의 `-K` 옵션으로 생성되는 `.packed.zip`은 표준 단일 엔트리 ZIP/Deflate 파일입니다. 다만 ZIP 내부 데이터가 전처리된 BIN이므로 DDS 복원은 전용 디코더가 담당합니다.

## 요구 사항

- Windows
- C++17 이상을 지원하는 컴파일러
- `miniz.h`와 `miniz.c`
  - Scanline/Hilbert 후보 크기 비교에 사용합니다.
  - `encoder.cpp`와 같은 디렉터리에 배치하십시오.
- pigz
  - `pigz.exe`와 필요한 런타임 DLL을 `encoder.exe` 옆에 배치하거나 `PATH`에 추가하십시오.
  - 또는 `PIGZ_EXE` 환경 변수에 실행 파일의 전체 경로를 지정할 수 있습니다.
- [7-Zip](https://www.7-zip.org/)
  - 디코더의 ZIP/7Z 해제와 선택적인 7Z 인코딩에 사용합니다.
  - 기본 경로는 `C:\Program Files\7-Zip\7z.exe`입니다.

## 빌드

Visual Studio Developer Command Prompt에서:

```bat
cl /std:c++17 /EHsc /O2 encoder.cpp miniz.c /Fe:encoder.exe
cl /std:c++17 /EHsc /O2 decoder.cpp /Fe:decoder.exe
cl /std:c++17 /EHsc /O2 overhead_benchmark.cpp miniz.c /Fe:overhead_benchmark.exe
```

`overhead_benchmark.cpp`는 내부에서 `encoder.cpp`를 포함하므로 벤치마크를 빌드할 때 `encoder.cpp`를 명령행에 다시 추가하지 마십시오.

병렬 STL 구현에 따라 추가 런타임 라이브러리가 필요할 수 있습니다.

## 인코더 CLI

```text
encoder.exe <input.dds|folder> [output_folder] [pigz|7z] [level=7]
```

### pigz ZIP

pigz 모드는 레벨 `7`, `8`, `9` 중 하나를 선택할 수 있습니다. 레벨이 높을수록 일반적으로 압축률이 조금 좋아지고 인코딩 시간이 길어집니다. 기본값은 `7`입니다.

폴더 전체를 pigz 레벨 7로 인코딩:

```bat
encoder.exe "C:\Textures" "C:\Encoded" pigz 7
```

pigz 레벨 8:

```bat
encoder.exe "C:\Textures" "C:\Encoded" pigz 8
```

단일 DDS를 pigz 레벨 9로 인코딩:

```bat
encoder.exe "C:\Textures\texture.dds" "C:\Encoded" pigz 9
```

기존 명령과의 호환을 위해 `zip`은 `pigz`의 별칭으로 처리됩니다.

```bat
encoder.exe "C:\Textures" "C:\Encoded" zip 7
```

예상 출력:

```text
texture.dds.packed.zip
```

### 7Z

7Z 모드는 레벨 `0`부터 `9`까지 지원합니다.

```bat
encoder.exe "C:\Textures\texture.dds" "C:\Encoded" 7z 9
```

예상 출력:

```text
texture.dds.packed.7z
```

입력 폴더에 하위 디렉터리가 있으면 출력 디렉터리에도 같은 상대 경로를 유지합니다. `_restored`가 파일명에 포함된 DDS는 폴더 일괄 처리에서 제외합니다.

## 디코더 CLI

```text
decoder.exe <archive|folder> [output_folder]
```

폴더 전체 복원:

```bat
decoder.exe "C:\Encoded" "C:\Restored"
```

단일 파일 복원:

```bat
decoder.exe "C:\Encoded\texture.dds.packed.zip" "C:\Restored"
```

복원된 DDS는 인코딩 전 원본과 바이트 단위로 동일해야 합니다.

## 오버헤드 측정

```text
overhead_benchmark.exe <input.dds|folder> [raw_csv=overhead_raw.csv] [pigz_level=7] [runs=5] [decoder.exe]
```

기본 반복 횟수는 파일당 `5회`입니다. 각 파일을 5회 처리한 뒤 파일별 평균을 별도의 summary CSV에 기록합니다.

전체 폴더를 pigz 레벨 7로 5회 측정:

```bat
overhead_benchmark.exe "C:\Textures" "C:\Results\overhead_raw.csv" 7 5
```

pigz 레벨 8을 5회 측정:

```bat
overhead_benchmark.exe "C:\Textures" "C:\Results\overhead_level8.csv" 8 5
```

pigz 레벨 9를 5회 측정:

```bat
overhead_benchmark.exe "C:\Textures" "C:\Results\overhead_level9.csv" 9 5
```

출력 파일:

- 지정한 raw CSV: 매 실행의 원시 측정값
- `<raw_csv이름>_summary.csv`: 파일별 5회 평균값

측정 항목:

- `preprocess_ms`: Scanline/Hilbert 후보 비교와 최종 BIN 생성 시간
- `secondary_compress_ms`: 전처리된 BIN을 pigz ZIP으로 압축하는 시간
- `total_encode_ms`: 전처리와 2차 압축 시간의 합
- `total_decode_ms`: ZIP 해제부터 DDS 파일 복원 및 쓰기까지의 전체 시간
- `compressed_bytes`, `ratio`: 압축 크기와 압축률
- `verified`: 복원된 DDS가 원본과 바이트 단위로 동일한지 여부

측정 중에는 영상 재생, 게임, 컴파일, 다른 압축 작업처럼 CPU와 메모리를 많이 사용하는 프로그램을 실행하지 않는 것을 권장합니다.

## 지원 범위 및 주의 사항

- BC1/DXT1, BC3/DXT5, BC4/ATI1 계열을 대상으로 작성했습니다.
- 128바이트 DDS 헤더를 전제로 합니다.
- DX10 확장 헤더가 있는 DDS는 현재 지원하지 않습니다.
- 밉맵 또는 배열·큐브 텍스처처럼 여러 서브리소스가 연속 저장된 DDS는 별도 검증이 필요합니다.
- 인코더와 디코더는 동일한 `ScanAlgorithms.hpp` 구현을 사용해야 합니다.

## 종료 코드

- `0`: 모든 파일 처리 및 검증 성공
- `1`: 인자, 입력 경로 또는 필수 프로그램 오류
- `2`: 하나 이상의 파일 처리 실패
- `3`: 오버헤드 측정에서 복원 검증 실패

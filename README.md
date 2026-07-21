# Texture Compress

DDS의 BC 압축 블록을 압축 친화적인 순서와 구조로 재배치한 뒤 ZIP 또는 7Z로 저장하고, 이를 다시 원본 DDS 바이트 배열로 복원하는 Windows용 C++ 인코더/디코더입니다.

이 저장소의 압축파일은 일반 DDS를 단순히 ZIP/7Z로 묶은 파일이 아닙니다. 내부에 전처리된 사용자 정의 BIN 데이터가 들어 있으므로 원본 DDS로 복원할 때 반드시 이 저장소의 디코더를 사용해야 합니다.

## 파일

- `encoder.cpp`: DDS 파일 또는 폴더를 `.packed.zip`/`.packed.7z`로 인코딩합니다.
- `decoder.cpp`: 인코더가 만든 압축파일을 원본 DDS로 복원합니다.
- `ScanAlgorithms.hpp`: Scanline 및 직사각형 텍스처용 Hilbert 인덱스를 제공합니다.

## 처리 방식

인코더는 각 DDS에 대해 다음 작업을 수행합니다.

1. 128바이트 DDS 헤더와 BC 블록을 읽습니다.
2. Scanline과 Hilbert 순서의 예상 압축 크기를 `miniz`로 동시에 계산합니다.
3. 더 작은 결과를 만드는 순서를 선택합니다.
4. BC 블록의 엔드포인트와 인덱스 데이터를 압축 친화적으로 분리·재배열합니다.
5. 전처리 결과를 7-Zip으로 ZIP 또는 7Z 압축합니다.

디코더는 압축을 해제하고 저장된 정렬 방식에 따라 블록을 원래 위치로 돌려놓은 뒤 DDS를 생성합니다.

## 요구 사항

- Windows
- C++17 이상을 지원하는 컴파일러
- [7-Zip](https://www.7-zip.org/) 기본 경로 설치
  - `C:\Program Files\7-Zip\7z.exe`
- `miniz.h`와 `miniz.c`
  - 인코더의 Scanline/Hilbert 후보 크기 비교에 사용됩니다.
  - 두 파일을 `encoder.cpp`와 같은 디렉터리에 배치하십시오.

## 빌드

Visual Studio Developer Command Prompt에서:

```bat
cl /std:c++17 /EHsc /O2 encoder.cpp miniz.c /Fe:encoder.exe
cl /std:c++17 /EHsc /O2 decoder.cpp /Fe:decoder.exe
```

병렬 STL 구현에 따라 추가 런타임 라이브러리가 필요할 수 있습니다.

## 인코딩

```text
encoder.exe <input.dds|folder> [output_folder] [zip|7z] [level=9]
```

폴더 전체를 ZIP 레벨 9로 인코딩:

```bat
encoder.exe "C:\Textures" "C:\Encoded" zip 9
```

단일 DDS를 7Z로 인코딩:

```bat
encoder.exe "C:\Textures\texture.dds" "C:\Encoded" 7z 9
```

예상 출력:

```text
texture.dds.packed.zip
```

입력 폴더에 하위 디렉터리가 있으면 출력 디렉터리에도 같은 상대 경로를 유지합니다. `_restored`가 파일명에 포함된 DDS는 폴더 일괄 처리에서 제외합니다.

## 디코딩

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

복원 결과:

```text
texture.dds.packed.zip -> texture.dds
```

## 지원 범위와 주의 사항

- BC1/DXT1 계열, BC3/DXT5, BC4/ATI1 데이터를 대상으로 작성되었습니다.
- 레거시 128바이트 DDS 헤더를 전제로 합니다.
- DX10 확장 헤더가 있는 DDS는 현재 지원하지 않습니다.
- 밉맵이나 배열·큐브 텍스처처럼 여러 서브리소스가 연속 저장된 DDS는 별도 검증이 필요합니다.
- 압축파일을 신뢰할 수 없는 입력으로 사용할 경우 7-Zip 압축 해제 단계의 경로와 내용도 별도로 검증해야 합니다.
- 인코더와 디코더는 동일한 `ScanAlgorithms.hpp` 구현을 사용해야 합니다.

## 출력 코드

- `0`: 모든 파일 처리 성공
- `1`: 인자, 입력 경로 또는 필수 프로그램 오류
- `2`: 하나 이상의 파일 처리 실패


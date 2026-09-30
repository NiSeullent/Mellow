# macOS 실기 시험용 소스 패키지

`build/mellow-apple-userspace-source.zip`은 네이티브 사용자 공간 소스와 시험 클라이언트의 전달 패키지다. 실행 바이너리나 완성된 미지원 GPU kext가 아니다. 이 Linux 작업 환경에서 macOS 사용자 공간 SDK 컴파일과 GPU 실행은 수행하지 않았다.

압축을 풀면 `mellow-apple-userspace-source/`에 실제 Runtime 소스, AppleMetal 어댑터, IOSurface/AppKit 표시 경로, 시스템 Metal 진단 도구, 필요한 시험 fixture와 원본 라이선스·출처 문서가 있다. `source-manifest.json`은 포함 파일 각각의 SHA256과 소스 commit을 기록한다. 패키지는 Apple SDK·드라이버·firmware를 포함하지 않는다.

기존 SDK가 설치된 Intel Mac에서 아래 명령으로 빌드할 수 있다. 빌드 출력은 소스 폴더와 겹치지 않는 새 폴더를 사용한다.

```sh
cd mellow-apple-userspace-source
python3 Tools/build-apple-userspace.py --out ../mellow-native-15 --deployment-target 15.0
```

macOS 26용 별도 최소 버전 빌드는 출력 폴더와 `--deployment-target`을 각각 `../mellow-native-26`, `26.0`으로 바꾼다. 빌더는 기존 SDK를 찾고 소스·바이너리 해시와 실제 Mach-O 버전·아키텍처를 확인한다. 기본 동작은 빌드뿐이다.

실제 GPU 계산·렌더링과 앱 창 표시를 함께 시험하려면 새 출력 폴더에서 명시적으로 실행한다.

```sh
python3 Tools/build-apple-userspace.py --out ../mellow-native-acceptance --deployment-target 15.0 --run-compute --run-metal-render --run-render --opencl-gpu-index 0 --frames 90 --interval-ms 33 --timeout 60
```

결과는 출력 폴더의 `apple-userspace-build.json`과 세 클라이언트의 `*-acceptance.json`에 남는다. 실제 계산 결과, GPU event와 fence, 렌더 6144픽셀, IOSurface와 AppKit 표시 제출을 각각 검사한다. 빌드 실패의 실제 컴파일 진단도 같은 보고서에 남는다. 없는 장치와 실제 처리 오류를 별도 상태로 기록한다.

시스템 Metal 등록 구현에 필요한 실제 클래스·타입·ivar 배치와 registry 연결은 별도 읽기 전용 실행으로 수집한다.

```sh
../mellow-native-15/metal-inventory > ../mellow-native-15/metal-inventory.json
```

현재 실행 경로에는 기존 가속 OpenCL/CGL GPU 드라이버가 필요하다. 드라이버가 없는 RTX 또는 Intel GPU는 이 패키지만으로 초기화되지 않는다. `PASSED_LIMITED_APP_SCOPE`도 앱용 기능의 통과이며, 전체 모델의 시스템 Metal·WindowServer 가속을 뜻하지 않는다. 자세한 계약과 실행 조건은 [네이티브 빌드 문서](APPLE-USERSPACE-NATIVE-BUILD.md)와 [시스템 ABI 상태](../Userspace/WindowServer/ABI-CONTRACT.md)에 있다.

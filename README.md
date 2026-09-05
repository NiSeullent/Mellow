# Mellow 7D41 0.4.1 실기 계측 개발판

**Metal 가속은 미완성이고 실제 부팅·설치·GPU 실행은 미검증이다.**
현재 legacy TGL 실험 경로와 새 native Xe backend는 별도 경로다.
새 backend는 device owner가 연결되지 않았다. 0.4.1은 이 상태를 PCI/IORegistry
진단에 공개하고 미구현 backend 요청을 거부한다. 기존 TGL 실험 요청은 유지한다.

Compute `x*7+3`, fresh nonce, 장치 귀속, GPU 완료·timestamp, offscreen render와
반복 제출의 검증 도구를 포함한다. 클라이언트는 누락된 Metal 드라이버를 대체하지 않는다.
현재 USB에서는 Mellow가 활성화되어 있으며 WhateverGreen은 비활성화되어 있다.

- [실제 빌드 검증](docs/BUILD-VALIDATION.md)
- [부팅·compute·render·장시간 제출 절차](docs/ACCEPTANCE-0.4.1.ko.md)
- [실제 backend 연결 감사](docs/NATIVE-XE-BACKEND-AUDIT.md)

아래는 0.4.0 구현 범위의 역사적 설명이다. 당시 기본 EFI/해시/검사 횟수는
그 스냅샷의 기록이며 현재 배포 상태는 위 문서를 따른다.

# Mellow 7D41 0.4.0 통합 개발판

Samsung Galaxy Book 750XHD, Core Ultra 7 255U, 물리 PCI GPU `8086:7D41`,
Tahoe / Darwin 25용 로컬 개발 소스와 실제 kext 빌드다. 원본은
[NiSeullent/Mellow](https://github.com/NiSeullent/Mellow)의
`f895d89653ea5cad3a534cce46a62d0f59384d71`이다. 0.4.0은 로컬 버전이다.

**실기 검증 전 개발판이며 완성된 Metal 가속 드라이버는 아니다.** 이번 버전에는
GuC firmware 적재·인증 상태 머신, MTL 컨텍스트·compute 명령과 실행 coordinator,
IOKit 인터럽트·실제 메모리 fence 처리, 공개 Metal API의 사용자 공간 연결 코드가 추가됐다.
실기 GPU에 적재하거나 실행한 결과는 없으며, 이 코드는 기본 EFI에서 활성화하지 않는다.

## 0.4.0 추가 구현

- [GuC firmware](docs/XE-GUC-FIRMWARE.md): 고정 Intel blob의 SHA256·CSS 검증,
  실제 GGTT PTE readback, reset·WOPCM·boot parameter·RSA pointer·DMA와 인증 상태 polling.
  실패 후 DMA backing을 유지하고 확인된 reset 이후에만 해제한다.
- [컨텍스트·dispatch·실행](docs/XE-CONTEXT-EXECUTION.md): 실제 MTL LRC 및 Intel
  명령 구조의 제한된 compute dispatch, VM 소유권, GuC context action, fence 기반 수명 관리.
- [IRQ / fence](docs/XE-INTERRUPT-FENCE.md): tile/master/identity 인터럽트 처리,
  IOFilterInterruptEventSource와 workloop, coherent GGTT qword fence 판독 및 timeout 격리.
- [Metal 사용자 공간 연결](docs/METAL-USERSPACE.md): 실제 MTLDevice와 PCI 장치 대조,
  Objective-C/IOKit 호출, MSL 컴파일, 공유 버퍼와 명령 제출, 완료·출력 검증 및 timeout 수명 관리.
  이 모듈은 공개 Metal 클라이언트다. 누락된 Metal 사용자 공간 드라이버를 만들거나 등록하지 않는다.

현재 물리 장치 backend에는 GGTT 예약·PTE publication·TLB invalidation 소유자,
MCR-aware PAT 및 전체 submission ADS·golden context·workaround 검증이 필요하다.
Tahoe의 사설 IOAccelerator/Metal 사용자 공간 드라이버와 셰이더 컴파일러 통합은 미완성이다.
실제 Intel Metal 번들의 Info.plist와 보조 라이브러리만 조사했으며, 드라이버 본체는 미확보다.
필수 근거가 없는 backend는 성공을 보고하지 않는다.

## 구현된 코드

- [XeMemory](docs/XE-MEMORY.md): 48-bit VA, owner/generation, 예약 구간,
  46-bit DMA, PTE/PDE, pin/bind/retire, 사용 참조·fence 기반 회수와 실패 격리.
  XeMemoryIOKit은 장치별 IOMapper 및 실제 IODMACommand로 할당·pin·동기화한다.
- [XePageTable](docs/XE-PAGE-TABLE.md): 4단계 테이블 구성·조회·해제, 풀 검증,
  부분 생성 rollback, seal. XePageTableIOKit은 실제 고정 DMA 메모리에 연결하며
  root 반환 전 동기화한다. 반환한 root는 사용 종료가 입증되어야 해제할 수 있다.
- [MMIO / forcewake](docs/XE-MMIO.md): 실제 BAR0 mapping, bounds/ordering,
  하드웨어 GMD_ID 판독, IP12.70 제한, GT/render forcewake, ACK·시간 제한·
  참조 수·실패 정리와 detach 수명 관리.
- [Firmware / Submission](docs/XE-SUBMISSION.md): 공식 GuC CSS 해석,
  immutable 제출 snapshot, owner/epoch/context/engine 검사, 제한된 시험 명령,
  timeout·late fence·reset·IRQ 배출 조건. XeMemorySubmission은 실제 VM의
  Bound 상태와 사용 참조를 queue에 연결한다. backend 완료를 만들어 내지 않는다.
- [GuC transport](docs/XE-GUC-TRANSPORT.md): CTB/HXG packet, ring wrap,
  response credit, cookie/epoch, BUSY·retry·timeout·late reply, context action.
  IOKit adapter는 MMIO로 6개 SELF_CFG와 CONTROL_CTB 응답을 확인한 뒤 연결한다.
  firmware identity·인증·GGTT backing 증거가 없으면 쓰기 전에 중단한다.
  CT 응답은 GPU 작업 완료 fence가 아니다.
- [Zebin loader](docs/XE-ZEBIN.md): 실제 Intel 컴파일 결과의 ELF64·section·symbol·
  relocation·metadata 검증과 ISA/data/BSS·커널 인자 배치. 정확한 mellow_evidence
  프로필에 제한되며, 임의 Metal shader loader는 아니다. 0.4.0의 제한된 compute walker는 별도 XeDispatch에 있다.
- [Tahoe 바이너리 검사](docs/TAHOE-ABI.md): 실제 Apple 복구 이미지의 kernel
  collection, KPI별 export whitelist·alias·버전과 Lilu를 대상으로 import 비교.
  Metal/CoreDisplay/IOAccelerator export와 메서드 문자열도 수집한다. 이름·버전
  일치는 private C++ ABI나 실제 호출 검증을 대체하지 않는다.
- [Metal 도구](docs/IOACCEL-METAL.md): 공식 Intel 오프라인 컴파일러 실행,
  ABI inventory, Swift compute/render probe, registry·nonce·출력 검증기.
  GPU 이름이나 MTLDevice 열거만으로 가속 판정을 통과시키지 않는다.

## 실제 입력과 검증 범위

공식 ocloc 26.27.39122.11 / IGC 2.38.2로 `-device 0x7d41` OpenCL C를
6,944-byte Zebin 및 1,636-byte SPIR-V로 컴파일하고 EU 역어셈블을 확인했다.
컴파일러가 선택한 mtl-u-a0는 실제 GMD_ID/stepping 측정이 아니다.
이는 **OpenCL C → EU 코드 생성**이며 Metal MSL frontend가 아니다.
바이너리와 보고서는 [compiler-evidence](compiler-evidence/compiler-report.json)에 있다.

공식 linux-firmware 고정 commit의 GuC 파일 320,320 bytes를 production parser로
읽고 해시·길이·버전을 확인했다. GuC 70.53.0 / submission ABI 1.26.0이다.
장치 RSA 인증 완료를 뜻하지 않는다. blob은 작업 폴더에 보존하며 패키지에는
출처·license·해시·재다운로드 도구를 포함한다.

[통합 검사](validation/xe-tests.json)는 실제 소스를 컴파일하고 호스트에서 VM,
page table, pin 수명, forcewake, GuC protocol, Zebin loader를 실행한다.
OS·GPU 경계는 명시적인 테스트 backend다. 반복·오류 주입 검사를 포함하므로
assertion 개수는 지원 기능 개수가 아니다. IOKit adapter는 커널 타깃으로 교차
컴파일했지만 macOS에서 pin/MMIO/IRQ를 실행한 결과는 아직 없다.

[빌드 검사](docs/BUILD-VALIDATION.md),
[Tahoe import 비교](abi-evidence/tahoe-import-resolution.json),
[parser 오류 주입 검사](abi-evidence/tahoe-parser-tests.json)를 함께 확인한다.

## 재현 및 설치 USB

현재 전체 호스트 검사는 `Tools/run-runtime-tests.py --cxx <C++ 컴파일러 경로>
--out <검사 출력 폴더> --firmware <공식 GuC 파일> --baseline`으로 실행한다.
[통합 실행 결과](validation/runtime-tests.json)에 13단계의 명령·종료 코드·입력 해시를 기록했다.
교차 빌드 명령·도구 출처는 [cross-build-notes.md](Tools/cross-build-notes.md)에 있다.
macOS에서는 Tools/metal-run.py와 metal-probe.swift로 실제 가속을 검사한다.
구형 1024개 요소 probe는 metal-probe-legacy.swift로 보존했다.

별도 GalaxyBook-Tahoe-USB 산출물은 SanDisk 64GB에 기록한 OpenCore + Apple
온라인 Recovery 설치 매체다. 네트워크가 필요하며 실제 부팅·설치 완료는 미검증이다.
기본 EFI는 -igfxvesa를 사용한다. 본 개발 kext는 USB 기본 EFI에서 자동 로드하지 않는다.
ROM 패치나 PCI ID 변경은 누락된 Metal 실행 스택을 제공하지 않는다.

MANIFEST.json, SHA256SUMS, changes-from-upstream.patch가 최종 스냅샷을 기록한다.
METAL-IMPLEMENTATION-PLAN.md 및 tests/xe_submission_provenance.json의 이전 미구현 목록은 과거 단계의 기록이다. 현재 범위는 이 README와 0.4.0 구성요소 문서를 따른다.

# EFI.zip — Samsung 255U / Sequoia 개발·진단 패키지

> **Metal 풀가속 완성본이 아닙니다.** 실제 OpenCore EFI와 네이티브 빌드한 Mellow.kext 0.4.4를 포함하지만, 이 삼성 노트북에서의 Sequoia 부팅·커널 로드·GPU 실행은 검증하지 못했습니다. **현재 코드에는 완성된 7D41 네이티브 GPU 제출 경로와 Apple Metal 드라이버 ABI가 없습니다.** 설치·가속 성공으로 표시하거나 판매할 수 있는 결과물이 아닙니다. 상세 상태는 `STATUS.json`에 기계 판독 가능한 형태로 적었습니다.

## 대상과 포함물

대상은 업로드한 SysReport의 **SAMSUNG NT751XHD-KR735 / Intel Core Ultra 7 255U / PCI 8086:7D41, subsystem C906144D / 내장 패널 1920×1080**입니다. Intel 공식 제품명상 내장 그래픽은 4 Xe 코어의 **Intel Graphics**입니다. “Iris Xe”라는 통칭보다 실제 PCI ID를 기준으로 개발했습니다.

`EFI/`는 USB의 EFI 시스템 파티션에 복사할 구조입니다. OpenCore 1.0.7, OpenRuntime, OpenHfsPlus, Lilu, VirtualSMC, SMCBatteryManager, NVMeFix, VoodooPS2 키보드 드라이버, RealtekRTL8111, Mellow.kext와 직접 생성·컴파일한 SSDT 3개가 들어 있습니다. **macOS 설치 프로그램·복구 이미지·Apple GPU 드라이버 바이너리는 포함하지 않았습니다.**

`Sources/`에는 해당 커밋의 Mellow 전체 소스, 작성한 SSDT 소스, 재배포 드라이버의 고정된 대응 소스가 있습니다. `dependencies.lock.json`에는 실제 다운로드한 릴리스·소스 커밋·SHA-256을 기록했습니다. `Licenses/`에는 원 저작권·라이선스를 보존했습니다. `Verification/`에는 실제 CI 빌드·구조 검사·컴파일·호스트 테스트 결과가 있습니다.

공개 저장소: https://github.com/NiSeullent/Mellow/tree/sequoia-255u-20260925
정확한 소스 커밋과 CI 실행 주소는 `STATUS.json`에 있습니다. GitHub의 같은 개발 릴리스에도 EFI.zip을 게시합니다.

## 이번에 실제로 추가한 구현

**커널 진단 경로:** 기존 Darwin 25 전용 서비스를 Darwin 24/25에 연결했습니다. `-mellowdiag`는 기존 그래픽 패치 콜백을 등록하지 않고 물리 PCI/BAR0 진단 경로만 선택합니다. 관리자 전용 selector 3은 요청 nonce, 실제 PCI·서브시스템·클래스·전원 상태, BAR 크기, GMD 12.70, PCI 레지스트리 ID를 반환합니다. 임의 MMIO 쓰기 인터페이스는 제공하지 않습니다. 관측 중 장치 상태가 달라지면 실패하며, 이전 성공 결과를 재사용하지 않습니다. GPU 제출·Metal 지원 플래그는 계속 false입니다.

**GGTT 관리 코드:** 고정 크기 예약·할당·PTE 발행·readback·무효화·참조·반납·격리 처리를 구현하고 실제 커널 빌드에 포함했습니다. 미확인 펌웨어 매핑을 덮어쓰지 않습니다. 그러나 물리 주소 공간 소유권, 실제 GGTT MMIO, TLB 완료, reset/retirement 증명을 공급할 통합 어댑터는 아직 없습니다. 기본 부팅에서 임의 GPU 명령이나 GuC 펌웨어를 실행하지 않습니다.

**사용자 공간 CGL 백엔드:** macOS의 실제 OpenGL 4.1 코어 컨텍스트, 오프스크린 렌더링, 펜스, RGBA readback을 사용하는 경로를 추가했습니다. 이는 **이미 가속 가능한 OpenGL 드라이버가 있는 Mac**에서 사용하는 제한된 MSL 호환 런타임이며, 7D41용 드라이버나 시스템 Metal 등록을 대신하지 않습니다. 소프트웨어 렌더러로 성공을 꾸미지 않습니다. 화면 표시·WindowServer·IOSurface·전체 Metal 명세는 구현 범위가 아닙니다.

**커널 빌드 수정:** 커널 개발 설정을 활성화하고 기존 명시적 MMX/SSE·자동 벡터화·red-zone 설정을 수정했습니다. 릴리스 실행 파일의 실제 디스어셈블 결과에서 SIMD 레지스터 사용을 검사합니다. plist 버전과 kmod 버전을 0.4.4로 맞췄습니다.

## 하드웨어 덤프를 반영한 EFI 설정

실제 노트북 EC인 `\_SB.PC00.LPCB.H_EC`를 보존하고 Darwin에서만 별도의 EC/USBX를 추가했습니다. USBX 전력 값은 관례적인 소프트웨어 설정이며 실제 포트 배선 측정값이 아닙니다.

원본의 48개 Device(PRxx) 중 MADT가 활성화한 **14개 UID만** Processor 객체로 추가했습니다. UID와 물리 x2APIC ID를 혼동하지 않았습니다. CP00의 plugin-type은 upstream PLUG-ALT 방식이며 BSP가 UID 0이라는 입증은 아닙니다. P/E/LP-E 3단계 토폴로지와 전력 관리는 실기기 검증 전입니다. 3단계 지원이 입증되지 않은 CpuTopologyRebuild를 기본 활성화하지 않았습니다.

원본 AWAC/RTC의 `_STA`가 실제로 참조하는 root GNVS 필드 `STAS`를 Darwin에서 1로 설정합니다. 임의 RTC 리소스 패치, 전체 `_OSI` 이름 변경, 전체 DSDT 교체는 하지 않습니다.

PS/2 키보드의 실제 호환 ID `PNP0303`에 맞춰 키보드 플러그인만 사용합니다. **I2C 터치패드를 PS/2로 가장하지 않습니다.** ELAN0B00/8086:7778/INTC105E 터치패드·GPIO, Wi-Fi 8086:7740, SST 오디오 8086:7728은 동작 보장 대상이 아닙니다. 설치 시험에는 유선 Ethernet과 외장 USB 키보드·마우스를 준비하십시오. USB 포트 맵은 덤프로 확정할 수 없어 생성하지 않았으며 XhciPortLimit은 껐습니다.

CPU CPUID EAX만 0x000906EA로 실험적으로 에뮬레이션합니다. GPU device-id/AAPL 플랫폼 ID는 주입하지 않습니다. 기본 `DisableIoMapper=true`이므로 기존 진단의 DMA 할당은 사용 불가로 나올 수 있습니다. 이는 Metal 실행 실패를 성공으로 바꿀 근거가 아닙니다. CPU·메모리 맵·CFG Lock 우회 설정은 물리 펌웨어에서 확인되지 않았습니다.

## 처음 시험할 때

**내장 디스크의 기존 EFI를 덮어쓰지 마십시오.** 기존 EFI·데이터를 별도로 백업하고, 독립 USB EFI 파티션으로 시험합니다. 복사할 것은 ZIP의 `EFI/` 폴더이며 `Sources`, `Tools`, 문서까지 ESP에 복사할 필요는 없습니다. 펌웨어 부팅 순서 자동 등록은 비활성화했습니다.

UEFI 모드, 펌웨어 Secure Boot 비활성 상태를 확인하십시오. ReBAR는 보고서와 동일하게 비활성 상태를 전제로 했습니다. 저장장치 모드·TPM·보안 설정을 무작정 초기화하지 마십시오. 기존 Windows의 복구 키가 필요한 변경은 별도 확인 후 진행해야 합니다.

기본 프로필은 verbose 진단용입니다. 화면 정지 또는 반복 부팅 시 기존 OS로 돌아가 USB의 `EFI/OC/config.plist`를 백업한 뒤 `Profiles/config-rescue.plist`로 교체합니다. 이 프로필은 Mellow를 끄고 `cpus=1`을 사용합니다. `Profiles/config-legacy-memory-map.plist`는 현대식 메모리 맵 설정에서 부팅 서비스 전환이 실패할 때 비교할 **미검증 대안**입니다. 여러 대안을 동시에 섞지 마십시오. 프로필 변경은 가속 구현을 추가하지 않습니다.

macOS에 진입하지 못하는 경우 이 패키지가 부팅 성공을 보장했다는 뜻이 아닙니다. 화면에 나온 마지막 메시지와 OpenCore 로그를 보존하고 시험 USB를 제거하여 원래 부팅 경로로 돌아갈 수 있어야 합니다.

## 공개용 SMBIOS 값을 그대로 실사용하지 마십시오

공개 배포본의 MacBookPro16,2 식별자는 **실험용 placeholder**입니다. Apple ID/iCloud/iMessage에 로그인하기 전에 로컬에서 적절한 고유 식별자를 준비하고 모든 프로필에 일관되게 적용해야 합니다. 타인의 실제 Mac 식별자를 복사하지 마십시오. 번들 `Tools/macserial`은 upstream 오프라인 도구이며 식별자 생성이 Apple 등록 상태·보증·서비스 자격을 확인한다는 뜻은 아닙니다.

`Tools/personalize-255u.py`는 사용자가 제공한 `serial`, `mlb`, `uuid`, `rom` 네 필드의 **로컬 비공개 JSON**을 읽어 압축 해제 폴더의 프로필에만 적용합니다. rom은 12자리 16진수입니다. 먼저 dry run으로 검사한 뒤 `--apply`를 명시합니다. 서버·GitHub·펌웨어로 업로드하지 않습니다.

```sh
python3 Tools/personalize-255u.py . --identity /비공개경로/identity.json
python3 Tools/personalize-255u.py . --identity /비공개경로/identity.json --apply
```

프로필을 수정한 뒤에는 배포 시의 `SHA256SUMS`와 달라지는 것이 정상입니다. 수정 전 원본 검증과 수정 후 설정 검증을 구분하십시오.

## 검사와 실기기 증거 수집

압축 해제 직후 원본의 파일 해시와 경로를 검사합니다. Windows에서도 Python 3로 실행할 수 있습니다.

```sh
python3 Tools/validate-255u-package.py . --verify-sha
```

원본·수정본 설정의 OpenCore 구조 검사는 OS에 맞는 `Tools/ocvalidate/ocvalidate`, `ocvalidate.linux`, `ocvalidate.exe`로 수행합니다. 구조 검사가 성공해도 하드웨어 부팅·그래픽 가속 성공은 아닙니다.

macOS 진입 후 실제 Mellow 진단 조회:

```sh
sudo ./Tools/sequoia-probe
sudo sh ./Tools/collect-mellow-logs.sh ./PRIVATE-MELLOW-EVIDENCE
```

수집 폴더는 자동 공개하지 않습니다. 로그에 개인 정보가 있는지 확인한 뒤 공유하십시오. 물리 진단 서비스가 없거나 상태 변화·D0·BAR 검사가 실패하면 도구가 실패 코드로 종료합니다.

`Tools/metal-probe` 및 `metal-run.py`는 **실제 Metal 장치, 물리 대상 귀속, nonce 기반 계산, 렌더링과 readback을 확인하기 위한 시험 도구**입니다. 현재 7D41에서 성공한다고 주장하지 않습니다. 장치 열거만으로 Metal 가속 완료로 판정하지 않습니다. 자세한 실행 방법과 기존 ABI 검증 도구는 `Sources/Mellow-source.tar.gz` 안의 Tools와 docs에 있습니다.

## 검증 범위와 참조 제한

실제로 실행한 검증 결과는 `Verification`의 개별 JSON·컴파일 결과를 보십시오. CGL 실행 시도는 클라우드 Mac에서 한 것으로, 실패/컨텍스트 없음 결과도 그대로 보존합니다. 호스트 모의 테스트와 실제 대상 GPU 시험을 같은 항목으로 합산하지 않았습니다. 과거 Windows GPU 시험 결과 역시 Sequoia/7D41 성공 증거가 아닙니다.

제공된 Miro 링크의 제목 **Mac OS X & Accelerated graphics**는 확인했지만, 보드 다이어그램 내용은 가져오지 못했습니다. 따라서 해당 구조의 모든 블록을 검증·완성했다고 주장하지 않습니다.

원본 SysReport, OEM ACPI 덤프, MSDM의 Windows 라이선스 키, 개인 Bluetooth 이름, 인증 정보, 비공개 작업 로그는 공개 ZIP과 GitHub 커밋에 넣지 않았습니다. 입력 파일의 SHA-256과 비개인 하드웨어 요약만 `hardware.json`에 보존했습니다.

주요 근거와 빌드·개발 설명: 공개 저장소의 `docs/SEQUOIA-255U.md`, `docs/XE-GGTT-OWNER.md`, `docs/SEQUOIA-CGL.md`, `EFI-255U/dependencies.lock.json`.

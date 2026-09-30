# 7D41 GuC ADS 구현과 검증 범위

`Mellow/XeGuCAds.*`는 물리 `8086:7D41`, graphics12.70의 main GT0를 위한
GuC Additional Data Structure 직렬화·검사 코드다. 메모리를 할당하거나
장치를 등록하지 않는다. 실제 PCI/펌웨어/GGTT/context를 보유한 드라이버의
권한·동기화 callback이 필수이며, 현재 그 통합 소유자는 제공되지 않는다.
**macOS GPU 실행·Metal·WindowServer 지원 완료를 뜻하지 않는다.**

## 구현한 부팅 단계

1. 소유자가 확인한 엔진·레지스터 저장 목록·capture 목록·WA 목록·doorbell
   값으로 전체 pre-load ADS를 구성한다. 없는 topology나 reg_sr를 추측하지 않는다.
2. GuC를 부팅하기 전에 mapping/masks/regsets/capture/WA와 빈 golden 예약을
   검사한다. Loader의 `preloadAdsValid`는 이 단계를 검증한다.
3. 부팅 후 실제 엔진의 WA 작업과 별도 NOP context로의 전환, 완료 fence와
   저장된 LRC를 소유자가 입증한 경우 해당 class의 golden image를 복사한다.
4. 모든 활성 class의 golden image와 원래 context/fence/epoch, 실제 DMA
   동기화를 새로 검사한 뒤에만 post-load 상태가 유효하다.
   Loader `submissionReady(owner,epoch)`는 실제 firmware 건강 상태와
   `goldenAdsValid`를 확인하고, 검사 후 건강 상태를 다시 읽는다.

부팅 전 검사에서 실제 GPU가 저장한 golden image를 요구하면 첫 부팅을
진행할 수 없다. 반대로 빈 예약과 firmware READY만으로 일반 작업 제출을
허용하지 않는다. CT 초기 설정·golden bootstrap과 일반 사용자 작업의
권한 구분은 통합 소유자가 구현해야 한다.

## 고정된 ABI와 제한

근거는 [Linux Xe 4d7d9486](https://github.com/torvalds/linux/tree/4d7d9486c04d917265f64c55bd23b2cc4fe7749c/drivers/gpu/drm/xe)의
[packed FWIF](https://github.com/torvalds/linux/blob/4d7d9486c04d917265f64c55bd23b2cc4fe7749c/drivers/gpu/drm/xe/xe_guc_fwif.h),
[ADS 초기화·post-load 저장](https://github.com/torvalds/linux/blob/4d7d9486c04d917265f64c55bd23b2cc4fe7749c/drivers/gpu/drm/xe/xe_guc_ads.c),
[capture 목록](https://github.com/torvalds/linux/blob/4d7d9486c04d917265f64c55bd23b2cc4fe7749c/drivers/gpu/drm/xe/xe_guc_capture.c)이다.
파일별 Intel MIT 저작권과 [허가문](../Drivers/PortedXe/LICENSE.MIT)을 보존한다.
저장소 전체의 LICENSE는 별도로 적용된다.

| 항목 | 현재 구현 |
| --- | --- |
| 펌웨어 | release70.53.0 / submission1.26.0. 실제 원본 hash/CSS·장치 인증은 Loader가 별도로 확인 |
| 고정 구조 | ADS4,572 bytes, policies96, system-info640, engine-usage16,384, UM128, prefix21,820 |
| regset | 8-byte header의 count는 u16. 실제 reg entry는16 bytes. physical instance로 인덱싱 |
| mapping | logical instance에서 physical instance로 연결; 미사용 값32 |
| 엔진 | main GT의 RCS0, BCS0, CCS0만 명시적으로 허용. 활성 mask는 실제 snapshot과 일치해야 함 |
| golden | RCS/CCS57,344 bytes·engine state52,864; BCS8,192·3,712 |
| 동적 배치 | 예약 regset 크기 뒤 page-aligned golden, WA, capture, firmware private data |
| WA | 해당 pin의 MTL WA `0x9002`·`0x900b`, 각 zero-length KLV |
| capture | 실제 PF global/class/instance 목록; BCS class의 명시적 빈 목록도 보존 |
| 제한 | media GT boot, 다른 Intel 계열·PCI ID, USM queues, indirect ring state 미지원 |

최대 3개 엔진과 제한된 목록 크기를 허용하며 전체 backing은 64 MiB 이하로
제한한다. 엔진의 HWS/IMR 두 레지스터가 있다는 것만으로 reg_sr가 완전하다고
판정하지 않는다. 실제 fused topology, 병합된 전체 reg_sr·capture·WA 및
firmware metadata의 완전성은 필수 `snapshotValid`가 입증해야 한다.

## 소유권과 저장 이미지

`Input`, DMA 페이지 배열, snapshot·capture cookie는 kernel 내부 전용이다.
배열을 읽기 전에 실제 소유권 callback을 확인한다. 할당 generation은
reset epoch와 구분하며, 정확한 pin/generation/IOMapper/CPU/GGTT 기록을
`regionOwned`가 검증해야 한다. 원본 LRC와 ADS가 DMA·CPU·GGTT 공간에서
겹치면 거절한다. 숫자로 된 context/fence/sequence만으로 capture를 인정하지 않는다.

golden 입력에는 실제 WA context와 전환 대상 NOP context의 서로 다른
fence·timeline·context·LRC와 소유자가 보존한 provenance가 필요하다.
capture 권한은 실제 작업 순서·전환·완료·CPU acquire visibility를 확인해야
한다. zero bootstrap LRC는 이 입력이 아니다.

활성 ADS를 읽기 전에 device-to-CPU 동기화와 권한 재검사를 한다. 원본 LRC
동기화 후, 실제 복사 직전에도 epoch·ADS 쓰기 권한·source 보유와 capture
증거를 재검사한다. 복사 후 device synchronization이 실패하면 자원을
보유한 실패 상태로 남긴다. 이 모듈의 소멸이나 timeout은 정리 증거가 아니다.

부팅 후 GuC가 갱신하는 engine saved value·usage·private data는 초기 0과
비교하지 않는다. capture 입력 descriptor와 golden 이미지·정책·mapping은
계속 엄격히 검사한다.

## 로컬 검증

[검증 기록](../validation/native-gpu/ads-lifecycle-checkpoint.json)은 검사한
소스와 객체·실행 파일의 SHA-256을 기록한다. ADS 시험의 topology·metadata·
DMA·fence·OS 경계는 명시적 시험 모델이며 `gpu_execution=false`다.
설치된 로컬 `mtl_guc_70.bin.xz`를 풀어 고정 원본 SHA-256과 일치함을 확인해
Loader 회귀도 실행했다. 다운로드·실기 firmware upload는 수행하지 않았다.

엄격한 Clang 경고·AddressSanitizer·UndefinedBehaviorSanitizer 검사 결과:
ADS166개, IOKit 트랜잭션406개, context execution1,266개,
forcewake3,134개, 원본 firmware Loader1,513개가 통과했다.
변경된 커널 소스7개는 Darwin24/25의 Mach-O 객체14개로 컴파일됐다.
이는 전체 kext 링크·적재나 Darwin IOKit 런타임 검사 결과가 아니다.

실제 완료 판정에는 native owner의 구현·전체 kext 링크·macOS 적재,
실물의 GuC/CT/WA/NOP/fence/capture 기록과 Metal·WindowServer 실행이 남아 있다.

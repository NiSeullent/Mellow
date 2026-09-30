# Intel·NVIDIA GPU 포팅: macOS 15 / 26

대상은 사용자가 지정한 Intel 미지원 그래픽 전체와 NVIDIA Maxwell 이후 전체이며, 운영체제는 macOS 15와 26이다. 이 범위는 개발 목표다. 현재 모든 모델의 가속을 제공하는 드라이버가 완성된 상태는 아니다.

이 작업 사본은 `codex/mellow-gpu-port-1edd`이며, 기준 commit은 `18d576866d6c2ae3543b5df2551c34dff8b8a45f`이다. 본 채팅은 Intel GuC transport, NVIDIA 명령 인코더, macOS용 Metal 호출 어댑터와 IOSurface 앱 표시 경로를 구현한다. 병렬 채팅의 source-family 계약 commit `cd187f4`는 비교 검토 후 `659ce58`로 통합했다. 아직 변경 중인 native memory/channel 코드는 해당 채팅의 소유 범위로 두고 후속 통합 대상으로 기록한다.

## 계열별 실행 경계

| 대상 | 커널 출발점 | 사용자 공간 출발점 | 필요한 Darwin 경계 |
|---|---|---|---|
| 최신 OS에서 제거된 구형 Intel | 해당 OS의 Apple binary와 i915 경로를 각각 검토 | 해당 세대의 compiler와 resource ABI | OS별 Apple ABI 호환성을 입증하거나 독립 backend 구현 |
| Intel Xe-LP: Tiger/Rocket/Alder/Raptor Lake, DG1 | family에 맞는 i915 또는 xe | Mesa Iris/ANV, Intel NEO | PCI/BAR, DMA, VM, firmware, context, IRQ/fence/reset |
| Intel Xe-HPG: Arc Alchemist | 해당 DG2 i915/xe 경로 | Mesa/NEO | discrete VRAM·BAR·migration과 firmware 인증 |
| Intel Xe-LPG: Meteor/Arrow Lake | 기존 Mellow Xe 연구 코드와 pinned xe | Mesa/NEO | GT/IP별 PAT·GGTT·GuC/GSC·실제 제출 owner |
| Intel Xe2 이후 | 각 family의 pinned xe | 해당 family를 지원하는 compiler/winsys | 명령·cache·VM·firmware를 세대별로 검증 |
| NVIDIA Maxwell/Pascal/Volta | Nouveau NVKM | Mesa Nouveau/NVK/NAK | Falcon/ACR, GPUVM, channel, Nouveau ABI, IRQ/fence/reset |
| NVIDIA Turing/Ampere/Ada | NVIDIA open RM/NVKMS 또는 Nouveau NVKM | 선택한 커널과 일치하는 provider | GSP·VM·submission과 RM 또는 Nouveau의 정확한 ABI |
| NVIDIA Blackwell 이후 | 해당 제품의 RM/Nouveau와 firmware boot 경로 | 해당 class와 일치하는 compiler/winsys | FSP/GSP 등 boot 경로를 별도 검증 |

NVIDIA open kernel modules는 Turing 이후 대상이다. Maxwell/Pascal/Volta를 이 backend에 추가하는 것으로 드라이버를 만들 수 없다. RM과 Nouveau/NVK의 memory·channel·submission ABI는 서로 다르다. Mesa가 macOS에서 컴파일된다는 사실도 Linux hardware driver의 Darwin 실행을 뜻하지 않는다.

## 실제 코드 변경의 관문

Intel transport에서는 최종 응답의 수신 여부를 credit ownership과 별도로 기록한다. FAST_REQUEST는 credit을 잡지 않는 경우에도 한 번의 실패 응답을 받을 수 있으므로, credit 유무만으로 중복 final 응답을 식별할 수 없다. 정상 BUSY 반복과 timeout 뒤 첫 late final은 유지하되, 이미 끝난 응답의 재수신은 기존 결과를 바꾸기 전에 transport fault로 처리해야 한다.

CT descriptor와 H2G/G2H buffer는 CPU 주소 범위에서도 겹치면 안 된다. 주소 끝 계산의 overflow와 구조적 overlap은 callback이나 memory reset 전에 검사한다. 이 검사는 실제 물리 alias, DMA coherency, GGTT publication, CTB stopped 상태를 입증하는 hardware owner의 책임을 대신하지 않는다.

NVIDIA의 독립 명령 인코더는 검토한 공식 class header의 GPFIFO와 method packet wire format만 제공한다. 주소·길이·정렬·class admission을 검증해 출력한다. GPU 채널, DMA mapping, pushbuffer의 명령 의미, doorbell, firmware, 제출 또는 완료를 제공하지 않는다. 공개 class 번호가 일치하는 것만으로 GPU 소유권이나 macOS 지원을 부여하지 않는다.

GuC와 NVIDIA 인코더 변경은 로컬 commit `52d35fa`에 보존했다. GuC production source는 105,180개 호스트 검사를 통과하고, 수정 전 소스에 같은 새 검사를 적용하면 중복 terminal 응답에서 실패한다. NVIDIA 인코더는 131,616개 검사를 통과한다. 두 모듈은 Clang 메모리 오류 검사와 macOS 15/26용 Darwin kernel object 컴파일을 통과했다. 이는 kext 링크·적재·실행 또는 OS별 ABI 검증 결과가 아니다.

실행 context의 종료 경로는 commit `f804910`에서 GuC 요청 기록을 반환하도록 수정했다. GPU 완료와 GuC 응답 credit 반환을 각각 확인하며, 아직 응답을 소유한 요청과 context/fence 핸들은 재시도할 수 있게 유지한다. 같은 transport에서 33개 작업을 완료하는 검사를 포함한 954개 검사가 Clang 메모리 오류 검사와 함께 통과했다. 원본 종료 코드에 새 검사를 적용하면 완료 후 요청 기록이 남는 문제가 재현된다. 변경된 context source도 두 OS 대상 kernel object 컴파일을 통과했다.

## 현재 구현한 macOS 사용자 공간 경로

`Userspace/AppleMetal`은 실제 `Runtime/MetalObjects`를 Objective-C Metal selector에 연결하는 명시적 앱용 어댑터다. shared uint32 buffer, 제한된 MSL compiler, 실제 OpenCL pipeline·dispatch·readback, 비동기 completion과 resource lifetime을 연결한다. 미구현 기능과 잘못된 resource/device를 거절하며 Apple GPU family나 전체 Metal protocol을 제공한다고 선언하지 않는다.

`MellowCreateRenderDevice`는 별도 CGL 렌더 장치를 Metal selector로 제공한다. BGRA8 IOSurface texture, vertex/fragment 함수와 pipeline, clear/store render pass, triangle draw, 비동기 completion 및 완료된 texture readback을 연결한다. compute/render 장치 간 공유는 제공하지 않는다. 네이티브 시험 클라이언트는 두 번의 GPU draw와 독립 픽셀 검사, 외부 texture 참조를 해제한 뒤의 유지 수명, 오류 입력 거절을 확인한다.

렌더 경로는 실제 CGL 4.1 context에서 `CGLTexImageIOSurface2D`로 연결한 BGRA8 IOSurface에 MSL 변환 결과를 draw한다. GPU fence와 GL readback, 공유 surface ID를 확인한 뒤에만 완료된 texture를 노출한다. 다음 쓰기가 실패하면 이전 완료 표식을 재사용하지 않는다. `RenderTexture::read()`는 top-left RGBA이고 raw surface는 bottom-left BGRA다.

`Userspace/WindowServer/Acceptance.mm`은 실제 렌더 픽셀을 독립 삼각형·gradient 계산으로 검증하고 IOSurface snapshot과 모든 채널·행을 비교한 뒤 전용 CALayer를 가진 AppKit 창에 표시한다. snapshot의 CPU 복사는 GPU 렌더와 구분해서 기록한다. 앱의 CATransaction 제출은 시스템 WindowServer가 Mellow GPU를 가속 장치로 채택했다는 증거가 아니다.

`Userspace/Diagnostics/MetalInventory.mm`은 실제 시스템 Metal device, IORegistry provider chain, Objective-C instance/class method type과 ivar 배치, display device 연결을 읽는다. private selector를 호출하거나 명시적 드라이버 적재를 요청하지 않는다. 일반 Metal 열거가 기존 provider를 내부 초기화할 수 있다는 경계도 기록한다. 측정한 타입과 exact OS build는 다음 Apple ABI 구현에 사용할 자료이며, method name만으로 잘못된 factory나 user-client layout을 만들지 않는다.

네이티브 빌드와 실기 실행 방법은 [APPLE-USERSPACE-NATIVE-BUILD.md](APPLE-USERSPACE-NATIVE-BUILD.md), 미해결 시스템 등록 ABI는 [ABI-CONTRACT.md](../Userspace/WindowServer/ABI-CONTRACT.md)에 있다. 현재 Linux 개발 환경에는 Foundation/Metal/OpenGL/IOSurface 사용자 공간 SDK가 없어 이 Objective-C++ 코드는 실제 Apple SDK 컴파일·실행 **NOT_RUN**이다.

이 앱용 경로는 macOS에 이미 존재하는 가속 OpenCL/CGL provider를 사용한다. provider가 없는 미지원 Intel·NVIDIA GPU를 직접 구동하는 kernel/firmware/VM/submission driver를 제공하지 않는다. 최종 성공 조건인 모든 대상 모델의 시스템 Metal·WindowServer 가속은 아직 충족되지 않았다.

통합된 source-intake의 18개 계열 profile은 소스 선택과 미구현 경계를 기록한다. 모든 모델을 망라하는 실행 지원표가 아니며, Hopper와 여러 구형 Intel 계열은 개별 profile도 아직 없다. `runtime_device_admission`과 `physical_gpu_verified`는 모두 false다. 소스 선택기를 추가하는 일과 실제 GPU를 구동하는 일은 별개 검증이다.

## macOS 15와 26의 수용 기준

두 OS는 각각 정확한 build 번호와 x86_64 kernel/KPI·IOAccelerator·사용자 공간 ABI를 기록해야 한다. 한 OS의 import 대응이나 object compilation을 다른 OS의 적재 성공으로 승격하지 않는다.

```mermaid
flowchart TD
    Identity["실제 PCI·subsystem·stepping / OS build"] --> Ownership["BAR·DMA·VM ownership"]
    Ownership --> Firmware["family별 firmware 인증과 boot"]
    Firmware --> Submission["context / channel / 실제 GPU 제출"]
    Submission --> Completion["IRQ·ordered fence·독립 readback"]
    Completion --> Recovery["fault·reset·전원 전환 검증"]
    Recovery --> App["Mellow opt-in compute / render"]
    App --> Metal["Apple Metal ABI / IOSurface"]
    Metal --> WindowServer["WindowServer / display / sleep"]
```

호스트 wire-format·lifecycle 테스트는 실제 production 알고리즘을 검증하지만, 시험 peer와 MMIO callback은 모델이다. 각 물리 GPU에는 firmware→submission→readback→reset 증거가 필요하다. 이후 앱용 기능, 시스템 Metal 등록, WindowServer, scanout, sleep/wake를 각각 검사한다. 초기화 실패를 성공 stub이나 CPU 결과로 바꾸지 않는다.

## 병렬 협업

공유 case 기록은 `/root/reverse-skill/work/mellow-gpu-port-1edd/COORDINATION.md`다. 각 채팅은 자기 작업 사본과 파일 담당 범위, source commit, 검증 결과를 기록한다. 소스 intake와 family 목록은 실제 실행 지원 목록이 아니다. 서로의 승인과 physical acceptance를 자동 상속하지 않는다.

## 1차 자료

- [NVIDIA kernel module architecture 범위](https://docs.nvidia.com/datacenter/tesla/driver-installation-guide/610/kernel-modules.html)
- [NVIDIA 공개 RM/NVKMS 소스와 OS 경계](https://github.com/NVIDIA/open-gpu-kernel-modules)
- [Linux Nouveau UAPI](https://docs.kernel.org/gpu/driver-uapi.html#drm-nouveau-uapi)
- [Mesa NVK](https://docs.mesa3d.org/drivers/nvk.html)
- [Mesa macOS의 hardware-driver 경계](https://docs.mesa3d.org/macos.html)
- [Intel Xe kernel driver](https://docs.kernel.org/gpu/xe/)
- [Intel Xe firmware](https://docs.kernel.org/gpu/xe/xe_firmware.html)
- [Intel Mesa ANV의 firmware 의존성](https://docs.mesa3d.org/drivers/anv.html)
- [Intel NEO와 Darwin 이식의 출발점](https://github.com/intel/compute-runtime/blob/master/ARCHITECTURE.md)
- [기존 Mellow 구현 상태](IMPLEMENTATION-STATUS.md)
- [Intel GuC transport의 pinned ABI](XE-GUC-TRANSPORT.md)

본 문서는 공개 자료와 로컬 소스의 개발 경계를 기록한다. 새로운 물리 GPU 가속 또는 macOS 드라이버 적재 성공을 보고하는 문서가 아니다.

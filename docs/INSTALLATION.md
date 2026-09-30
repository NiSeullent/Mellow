# macOS 터미널에서 설치·제거

**현재 native macOS GPU 실행·Metal·WindowServer 지원을 실기로 검증해 완료한 기기는 없습니다.**
이 설치 프로그램은 실제로 빌드한 개발용 파일을 배치합니다. 설치 성공은 그래픽 가속 지원이나
드라이버 적재 성공을 뜻하지 않습니다. 기기별 상태는 [README](../README.md)를 확인하세요.

대상은 **Intel x86_64 macOS 15 이상**입니다. Apple silicon과 Rosetta 실행은 거절합니다.
macOS 15·26에서 파일 설치에 필요한 도구는 시스템에 포함되어 있으며 Python, Xcode,
개발 도구 다운로드가 필요하지 않습니다. 해당 OS에서의 GPU·Metal·화면 표시 동작은 별도 검증 대상입니다.

## 기본 설치

[GitHub Releases](https://github.com/NiSeullent/Mellow/releases)에서
`Mellow-macos-x86_64.zip`, `Mellow-macos-x86_64.zip.sha256`, `install-mellow.sh`,
`uninstall-mellow.sh`를 같은 폴더에 받으세요. 먼저 스크립트 내용을 확인한 뒤 터미널에서 실행합니다.

```sh
cd ~/Downloads
bash install-mellow.sh --archive Mellow-macos-x86_64.zip \
  --checksum-file Mellow-macos-x86_64.zip.sha256
```

스크립트가 특정 릴리스의 ZIP과 checksum을 직접 받도록 할 수도 있습니다.
다음 태그는 첫 바이너리 개발 릴리스용이며, Releases에 해당 asset이 게시된 뒤 사용할 수 있습니다.

```sh
bash install-mellow.sh --version platform-v0.4.4-macos-binaries-20260930
```

다른 버전은 실제 릴리스 태그로 바꿉니다. 개발 릴리스는 GitHub의 `latest`에서 빠질 수 있으므로
버전을 명시합니다. ZIP을 선택하지 않고 실행하면 도움말을 표시하며 설치하지 않습니다.
스크립트를 받아 저장하고 검토한 뒤 실행하세요. 다운로드한 내용을 바로 `sudo`로 실행하지 마세요.

기본 위치는 `~/Library/Mellow`입니다.

| 위치 | 내용 |
| --- | --- |
| `~/Library/Mellow/Frameworks` | 앱이 명시적으로 선택하는 Mellow 사용자 공간 framework |
| `~/Library/Mellow/bin` | CLI와 인접한 `libMellowAppleUserspace.dylib` |
| `~/Library/Mellow/licenses` | 배포 파일의 라이선스와 고지 |
| `manifest.json`, `SHA256SUMS`, `.mellow-files` | 릴리스 정보·원본 checksum·설치 파일 기록 |

기본 설치는 관리자 권한을 요구하지 않습니다. shell 설정과 PATH를 바꾸지 않으며,
GPU 시험·창 표시·kext 적재·시스템 Metal 등록을 실행하지 않습니다.
파일을 배치한 뒤 기존 macOS Metal 장치를 읽기만 하는 진단은 다음과 같습니다.

```sh
"$HOME/Library/Mellow/bin/metal-inventory"
```

이 출력은 시스템이 이미 노출하는 Metal 장치의 목록입니다. Mellow가 드라이버 없는 GPU를
지원한다는 증거가 아닙니다. compute·render acceptance 프로그램은 실제 GPU 작업을 요청하므로
별도로 실행 여부를 결정해야 합니다. 설치 프로그램은 자동으로 실행하지 않습니다.

위치를 바꾸려면 `--prefix`에 절대 경로를 전달합니다. 같은 위치에 설치 프로그램이 관리하는
변경 없는 파일이 있으면 교체할 수 있습니다. 관리하지 않는 파일이나 수정한 파일이 있으면
덮어쓰지 않고 중단합니다. 시스템 폴더와 symlink를 포함하는 설치 경로는 거절합니다.

```sh
bash install-mellow.sh --archive ./Mellow-macos-x86_64.zip \
  --checksum-file ./Mellow-macos-x86_64.zip.sha256 \
  --prefix "$HOME/Library/Mellow-test"
```

## 선택: kext 파일 배치

현재 kext는 개발용이며 Developer ID 서명·공증이 없습니다. 일부 사용자 공간 파일에 있는
ad-hoc 서명은 개발자 신원 확인이나 공증을 대신하지 않습니다. macOS 보안 정책 때문에
정상적인 설치·적재가 거절될 수 있습니다. 사용자 승인과 재시작도 필요할 수 있습니다.
[Apple의 kext 안내](https://developer.apple.com/documentation/apple-silicon/installing-a-custom-kernel-extension)와
[시스템 확장 배포 안내](https://support.apple.com/guide/deployment/depa5fb8376f/web)를 참고하세요.

`--install-kext`를 명시하면 사용자 공간 파일에 더해 `/Library/Extensions/Mellow.kext`를 배치합니다.
이 단계만 `sudo` 인증을 요청합니다. Mellow는 **Lilu 1.6.4 이상**이 필요합니다.
기존 `/Library/Extensions/Lilu.kext`는 식별자와 버전만 확인하며 수정하지 않습니다.
버전이 부족하거나 다른 설치 프로그램이 만든 Mellow가 있으면 자동 교체하지 않고 중단합니다.
EFI 등 다른 위치의 Lilu 설치는 이 도구가 관리하거나 확인하지 않습니다.

Lilu가 없다면 `--install-dependencies`를 추가하여 릴리스에 포함된 Lilu의 동반 배치를 선택합니다.
사용자 공간 파일만 설치할 때는 Lilu가 필요하지 않습니다.

```sh
bash install-mellow.sh --archive ./Mellow-macos-x86_64.zip \
  --checksum-file ./Mellow-macos-x86_64.zip.sha256 \
  --install-kext --install-dependencies
```

macOS의 표준 cache 검사·준비까지 요청하려면 `--prepare-kext`를 추가합니다.
실행하는 명령은 `sudo /usr/bin/kmutil install --volume-root / --check-rebuild`입니다.
이 옵션은 OS 정책에 따라 실패하거나 사용자 승인을 요구할 수 있습니다. 자동 재부팅과
보안 설정 변경은 하지 않습니다. kmutil 실패 시 이미 설치한 파일은 남고 종료 코드 3을 반환합니다.
설치 프로그램은 SIP·Secure Boot·Gatekeeper를 해제하거나 인증 정보를 저장하지 않습니다.

## 제거

```sh
bash uninstall-mellow.sh
```

다른 위치에 설치했다면 같은 `--prefix`를 지정합니다.
설치 기록에 있는 **변경 없는 파일만** 제거합니다. 사용자가 추가한 파일과 수정한 파일은 남깁니다.
수정한 파일이 남으면 종료 코드 3을 반환하며, `.mellow-files`도 남겨 다음 제거 때 확인할 수 있게 합니다.
사용자 설치 폴더 전체를 재귀 삭제하지 않습니다.
사용자가 추가한 파일만 남은 경우는 정상 종료합니다. kext는 번들 식별자와 전체 등록 파일
구성이 일치할 때만 제거하며, 기록이 불완전하거나 추가·수정 파일이 있으면 번들과 기록을 보존합니다.

이 설치 프로그램이 배치한 kext도 제거하려면 다음과 같이 선택합니다.

```sh
bash uninstall-mellow.sh --remove-kext --prepare-kext
```

기본적으로 Lilu는 남깁니다. 이 도구가 설치한 Lilu까지 제거하려면 `--remove-dependencies`도
명시해야 합니다. 다른 확장이 Lilu를 사용할 수 있으므로 필요한 경우만 선택하세요.
기존에 있던 Lilu나 다른 도구가 설치한 kext는 제거하지 않습니다. 이미 적재된 kernel 코드를
강제로 내리지 않으며, 디스크에서 제거한 뒤 실제 반영에는 정상적인 재시작이 필요할 수 있습니다.

## 검증과 실패 처리

다운로드는 HTTPS로 임시 폴더에 저장하고, ZIP 전체 digest와 내부 `SHA256SUMS`를 검사합니다.
manifest의 패키지·저장소·아키텍처·OS 요구사항·실제 빌드 상태도 확인합니다.
ZIP에는 경로 이탈, 중복·대소문자 충돌, symlink와 특수 파일을 허용하지 않습니다.
서로 다른 릴리스의 ZIP과 checksum을 섞으면 설치하지 않습니다.

외부 checksum 없이 `--archive`만 사용하면 내부 파일의 일치 여부는 확인하지만 게시자 신원은
인증하지 못합니다. 별도로 확인한 ZIP digest를 `--sha256`으로 전달하거나 외부 checksum 파일을
사용하세요. 같은 HTTPS 릴리스에서 받은 checksum도 전송 일치 확인이며 디지털 서명은 아닙니다.

파일은 먼저 새 폴더에 복사하고 검사한 뒤 디렉터리 이름 변경으로 교체합니다.
여러 폴더를 함께 교체하는 과정 전체가 단일 원자적 작업은 아닙니다. 보통의 명령 실패·중단에서는
이전 파일을 복구하며, 복구가 불가능하거나 파일이 변경되었으면 보관 경로를 출력합니다.
전원 차단·강제 종료에서는 복구 trap이 실행되지 않을 수 있으므로 출력된 backup과 lock을
확인해야 합니다. 파일을 확인하지 않고 lock이나 backup을 자동 삭제하지 마세요.

`--help`는 아무 파일도 바꾸지 않습니다. 이 저장소의 설치 소스 작성만으로 실제 macOS 설치나
native GPU 동작을 검증한 것은 아닙니다. 게시된 빌드·설치 검사 기록과 기기별 실기 기록을 구분하세요.

# Sequoia / Core Ultra 7 255U development branch

## Delivery status

**Partial implementation, not a working full-Metal driver.** This branch continues the real Mellow source tree at `9fe59bb`, builds an actual x86_64 kernel extension, adds a Darwin 24 physical discovery path, and assembles a hardware-derived OpenCore EFI. It does not implement the missing native Xe submission owner, Apple Objective-C Metal driver ABI, or WindowServer integration. Neither Sequoia boot nor kext loading on the Samsung target has been tested. An OpenCore schema pass, a native Xcode build, and host tests must not be called hardware compatibility certification.

The target is Samsung NT751XHD-KR735, physical PCI 8086:7D41 / subsystem C906144D, Core Ultra 7 255U. Intel names this SKU's integrated GPU **Intel Graphics**, with four Xe cores. The physical PCI identity, rather than the informal Iris Xe name, selects the driver.

Source: [Intel 255U specification](https://www.intel.co.kr/content/www/kr/ko/products/sku/241860/intel-core-ultra-7-processor-255u-12m-cache-up-to-5-20-ghz/specifications.html). The uploaded report and OEM ACPI were reviewed privately; their hashes and only non-personal hardware facts are retained in `EFI-255U/hardware.json`.

## Changes in 0.4.4

The existing diagnostic IOService now admits Darwin 24 and 25 using a shared, tested admission function. `-mellowdiag` selects it without registering the legacy Apple graphics patch callbacks. `-mellowoff` suppresses both Lilu plugin initialization and diagnostic service admission. Conflicting legacy/native requests reject diagnostic matching.

A new administrator-only selector 3 returns a nonce-correlated, bounded physical sample: PCI identity, subsystem, class/revision, D0 status, BAR length, GMD 12.70 and PCI registry entry ID. It reads the physical configuration before any spoof. It exposes no arbitrary register interface or write operation. The existing v1 selectors 0–2 remain unchanged. The sample is not atomic, is not a reset epoch, and does not attest GPU execution. Unsupported, inaccessible or changing device state returns an error without reusing an earlier sample. `gpuSubmissionSupported` and `metalSupported` remain false.

The actual kernel Xcode build settings previously enabled MMX/SSE/vectorization while disabling the kernel-development setting. This branch enables the kernel setting, disables MMX/SSE/vectorization/red-zone use, and audits the executable disassembly. Module metadata and bundle version are synchronized at 0.4.4.

`XeGgtt` supplies a bounded production reservation/publication/retirement manager using the existing ported Xe PTE encoder. Its trusted physical allocator/MMIO/invalidation/retirement adapter is still required. See [the exact GGTT scope](XE-GGTT-OWNER.md).

The user-space provider gains a real Apple CGL 4.1 offscreen path. It needs an already accelerated OpenGL driver; it does not manufacture one for 7D41. See [CGL scope](SEQUOIA-CGL.md).

## Reference board

The supplied Miro board resolves publicly to **Mac OS X & Accelerated graphics**. Its public page and oEmbed expose metadata, but the picture resolves to a generic icon and no diagram contents were retrieved. Therefore this branch does **not** claim to have verified the diagram or implemented every block in it. Existing repository architecture and inspectable source contracts were used.

Reference: https://miro.com/app/board/uXjVHtjgrD4=/?share_link_id=288963542502

## Firmware-derived EFI decisions

The real laptop EC `\\_SB.PC00.LPCB.H_EC` is preserved. A separate Darwin-only fake EC and USBX power-properties device are additive. USBX budgets are conventional software values, not measured physical port capabilities. The original DSDT contains no EC or USBX name collision.

The original processor objects are 48 `Device(PRxx)` entries, not `Processor` opcodes. Only the fourteen enabled MADT UIDs are represented in an additive PLUG-ALT-style table. UID and physical x2APIC ID are distinct; the enabled UID-to-APIC mapping is retained. `plugin-type` on CP00 follows the upstream convention; the MADT does not prove which UID is the BSP. Three-tier topology and power management remain unverified. CpuTopologyRebuild is not silently enabled on this three-tier platform.

The original AWAC and RTC `_STA` methods explicitly choose between `STAS==0` and `STAS==1`. A Darwin-only table sets the existing root GNVS field STAS to one. No global `_OSI` rename or guessed RTC resource patch is used.

The keyboard already has PNP0303 as its compatible ID, so the official VoodooPS2 matching rule applies without a fake HID. The ELAN0B00 touchpad is I2C-HID on a different controller; it is not treated as a PS/2 touchpad. Wi-Fi 8086:7740, GPIO INTC105E, I2C 8086:7778 and SST audio 8086:7728 are not advertised as supported just because familiar marketing names appear in the report. No fabricated USB port map is supplied.

References:
- https://github.com/acidanthera/OpenCorePkg/blob/1.0.7/Docs/AcpiSamples/Source/SSDT-PLUG-ALT.dsl
- https://dortania.github.io/Getting-Started-With-ACPI/Universal/ec-fix
- https://github.com/b00t0x/CpuTopologyRebuild/issues/24
- Exact dependency revisions and license sources: `EFI-255U/dependencies.lock.json`.

## Reproducible tests

```sh
python3 Tools/run-255u-tests.py --cxx clang++ --out build/255u-tests
python3 Tools/run-tahoe-diagnostic-tests.py --cxx clang++ --sanitize --out build/diagnostic
python3 Tools/run-render-shader-tests.py --cxx clang++ --sanitize --out build/render-shader
python3 Tools/generate-255u-acpi.py --check
bash Tools/cross-build-native.sh Release # requires macOS + full Xcode
```

The CI retains the real compiler output, disassembly audit, host-test evidence and optional CGL attempt. A failed CGL context request is recorded as failure/unavailable, never substituted by a CPU result. The official full-Metal acceptance probe is included as a future hardware acceptance tool, not as a certificate that it currently succeeds.

## First-boot isolation profile

The package includes `Profiles/config-boot-baseline.plist` for a controlled first boot on an independent USB EFI. It is identical to `EFI/OC/config.plist` except that `Mellow.kext` is disabled and the `-mellowdiag` boot argument is replaced with `-mellowoff`. In particular, it keeps the same CPU count, CPUID emulation, ACPI tables, memory-map quirks, and other kexts. Compare a boot with this profile against the default profile before changing any other boot setting. Keep the existing `config-rescue.plist` with `cpus=1` for a separate fallback, and `config-legacy-memory-map.plist` for a separate memory-map comparison. A successful baseline boot alone does not prove Mellow caused a default-profile failure, and none of these profiles demonstrates GPU acceleration.

## Public-data boundary

Original SysReport, DSDT/SSDT dumps, MSDM, licensing keys, personal Bluetooth names, access tokens, and private agent logs are not release inputs. Own source/SSDTs, sanitized hardware identity, dependency hashes, binaries and actual bounded validation reports are public. This is not a macOS installer and contains no Apple GPU-driver binaries or recovery image.

## Continuation build corrections

The first native EFI workflow built the actual kext but stopped because Apple's Xcode
AddressSanitizer runtime rejected `detect_leaks=1`. The shared sanitizer policy now keeps
ASan and UBSan fatal on both macOS and Linux, enables leak detection on Linux, and explicitly
records macOS leak checking as not performed. No failed workload is retried without
instrumentation. Platform-policy regressions run before the native and Linux suites.

Release validation additionally checks every referenced UEFI driver/tool, bounded PE32+
and Mach-O headers, kext dependency order, the exact Mellow binary digest from the native
build receipt, and complete/unique manifest coverage. Negative controls corrupt filenames,
binary bytes, readiness claims and the checksum manifest. Assembly refuses a dirty source
checkout or a native artifact from a different commit. The original general log collector
is preserved; the scoped target collector is separately named `collect-255u-logs.sh`.

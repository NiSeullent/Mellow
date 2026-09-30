# GSP LibOS Radix3 image tables

This module builds the embedded-image DMA scatter table used before booting
NVIDIA GSP-RM. It ports a concrete format algorithm, with no memory allocation,
firmware acquisition, GPU register access, channel creation or boot operation.
No physical GPU, Metal, WindowServer or driver support follows from a successful
serialization test.

The source is NVIDIA's MIT-licensed
[kgspCreateRadix3_IMPL](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c#L5483)
at revision `e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb` (release 610.57.04).
The [LibOS header](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/common/uproc/os/common/include/libos_init_args.h#L43)
defines 4096-byte pages. Each table page holds 512 raw 64-bit DMA addresses.
Three table levels are mandatory even for a one-byte image. The root must fit
one page, so the format represents at most 512 GiB of payload; this is a format
bound, not a supported firmware size or device memory capacity.

`planRadix3` gives the page counts and byte offsets. `serializeRadix3` writes
the three address levels followed by the image, with zero unused table entries
and zero padding through the last 4 KiB image page. It preserves the caller's
logical page order; scratch sorting is only an alias audit. Root and intermediate
entries contain child table DMA addresses. Leaf entries contain image page DMA
addresses. They have no valid/aperture bits or address shift and are unrelated to
the GPU virtual-address PTE/PDE format owned by the VM implementation.

The caller supplies exact page lists for tables and payload from its actual
GPU-DMA mapper, the mapper's address width, private CPU output, immutable image
bytes and disjoint scratch. The function checks complete page intervals,
alignment, counts, CPU span overlap/overflow, corrupted layout and duplicate
physical pages before changing any firmware output. Zero raw addresses are
mechanically representable; an address value is never proof that the caller
owns, pins or can DMA from that page. Scratch may change on failure and is never
published. Output is unchanged on failure, including its unused capacity.

The implementation uses fixed local metadata and the caller's scratch, with
O(N log N) page-alias validation and no allocation, exceptions or STL containers.
The caller must keep all pages pinned and exclusively owned, perform actual
DMA/cache synchronization, and retain them until the GPU firmware consumer has
retired or reset has genuinely established DMA quiescence. The existing command
queue's GPU-VA mapping ownership alone does not establish this firmware DMA
lifetime. No invented owner flag or successful retirement callback is supplied.

The upstream image loader validates `.fwversion` against the matching driver,
extracts `.fwimage` and chip-selected signature bytes, then creates this table.
[WPR metadata](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/arch/nvalloc/common/inc/gsp/gsp_fw_wpr_meta.h#L69)
passes the table root address and image size to the Booter. Those steps, signed
firmware authentication, WPR carveout, FWSEC/FRTS, RISC-V reset/start, RPC queues,
actual GSP init-done response, channel allocation and reset are not implemented
by this byte serializer.

The [pinned NVIDIA README](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/README.md)
requires matching-release GSP firmware and userspace components and admits
Turing or later. This module therefore has no Maxwell/Pascal/Volta GSP support
claim. Historical Nouveau pre-Turing ACR/FECS/GPCCS firmware paths are distinct.

For a concrete comparison, pinned Linux/Nouveau
[GM200 graphics initialization](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/nouveau/nvkm/engine/gr/gm200.c#L208)
loads FECS and GPCCS bootloader/instruction/data/signature objects into ACR and
then loads graphics context and method initialization data. Missing graphics
firmware returns `-ENODEV`. Its
[GM200 ACR layout](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/nouveau/nvkm/subdev/acr/gm200.c#L122)
uses signed low-security Falcon images and a PMU bootstrap owner. The
[GP102 ACR path](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/nouveau/nvkm/subdev/acr/gp102.c#L72)
uses a different WPR header, SEC2 bootstrap owner and shadow/WPR allocation;
[GV100](https://github.com/torvalds/linux/blob/0d9ff90a5422cc7509258aaaba1e7481df4d332a/drivers/gpu/drm/nouveau/nvkm/subdev/acr/gv100.c)
inherits that layout with its own firmware interfaces. These sources establish
specific Maxwell 2/Pascal/Volta dependencies, not one common bootstrap for every
Maxwell model, and no GSP substitution is inferred.

The pinned NVIDIA
[Turing preparation](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c#L389)
checks the enabled RISC-V core. Its
[bootstrap](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/src/kernel/gpu/gsp/arch/turing/kernel_gsp_tu102.c#L480)
uses chip-dependent FWSEC/FRTS,
resets into RISC-V, programs LibOS boot arguments, executes the signed Booter,
sends initialization RPCs, links the status queue and waits for actual RM
initialization. The
[init-done handler](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c#L5790)
requires the real `GSP_INIT_DONE` event and successful RPC result; a CPU flag or
timeout cannot stand in for this response. Subsequent
[channel allocation](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/nvidia-uvm/uvm_channel.c#L2148)
receives actual runlist/channel resources from RM, and channel initialization
submits and waits for real engine/host work. Porting those owners and firmware
interfaces remains separate from the table serializer and command encoders.

The standalone test uses synthetic payload and DMA numbers, including fixed
one-page/513-page vectors, independent little-endian decoding, layout capacity,
full-address boundaries, corrupted input, CPU overlap/overflow, duplicate DMA
pages and unchanged output on failure. It loads no firmware and executes no GPU
work. A direct installed compiler invocation is:

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -pedantic -I . \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  Drivers/PortedNvidiaGsp/Radix3.cpp tests/ported_nvidia_gsp_radix3_test.cpp \
  -o /tmp/mellow-nvidia-gsp-radix3-test
/tmp/mellow-nvidia-gsp-radix3-test
```

Local verification on 2026-09-30 passed 195 synthetic checks with Clang 21.1.8
and ASan/UBSan, and the same 195 checks with installed GCC. The production source
also compiled as freestanding x86_64 Mach-O objects targeting macOS 15 and 26,
with compiler builtin headers, exceptions and RTTI disabled. No Apple SDK,
kext link/load, real firmware or physical GPU was used in those checks.

## Firmware container extraction

`FirmwareImage.hpp/.cpp` now implements `extractFirmwareImage`, a bounded,
allocation-free ELF metadata extractor. The caller provides immutable container
bytes, its matching driver release, the full signature section name selected
from actual chip/HAL/confidential-computing state, and section/string-table
resource budgets. Success returns borrowed version, image, selected signature
and raw build-id-note bytes, and changes no container bytes. Every failure
preserves the complete output view. Neither matching strings nor metadata
validation establish the source's trust, device compatibility or authentication.

The pinned [section extractor and loader](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c#L5289)
require `.fwversion` equal to the driver's `NV_VERSION_STRING` plus terminal NUL,
`.fwimage`, one selected signature and `.note.gnu.build-id`. The
[published definitions](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/generated/g_kernel_gsp_nvoc.h#L253)
identify ordinary `.fwsignature_` and confidential-computing `.fwsignature_cc_`
prefixes. The selector appends the real chip family string; the parser accepts
that exact caller-selected name and supplies no PCI-to-family table. The
[Hopper selector](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/src/kernel/gpu/gsp/arch/hopper/kernel_gsp_gh100.c#L49)
uses actual confidential-computing state. Upstream's selected-name buffer holds
32 bytes; this API accepts at most 31 name bytes. Its release profile accepts
1..62 text bytes, corresponding to upstream's less-than-64-byte diagnostic
version representation including the NUL. These are explicit parser profiles,
not hardware limits or a firmware compatibility guarantee.

The [LibOS ELF structures](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/common/uproc/os/common/include/libelf.h#L49)
give ELF64 header/section sizes of 64 bytes and a program-header size of 56 bytes.
This implementation decodes little-endian bytes without unaligned structure
casts. It strengthens the upstream reader's bounds and ambiguity checks:
ordinary indexes, exact header sizes, current ELF version, canonical zero
section header, bounded section/string/program tables, terminated names, all
nonempty-name duplicates and all file-content/metadata overlaps. Required
sections must contain uncompressed, nonempty file bytes. Empty names may repeat;
the caller's budgets bound the no-heap quadratic audit and do not set a chipset
limit. Extended section/program indexes are explicitly unsupported.

These range rules follow the ELF ABI's
[header](https://gabi.xinuos.com/elf/02-eheader.html),
[section](https://gabi.xinuos.com/elf/03-sheader.html) and
[string-table](https://gabi.xinuos.com/elf/04-strtab.html) definitions. In
particular, `SHT_NULL` and `SHT_NOBITS` contribute no file-content range; the
latter's conceptual offset/size are never used to access input bytes. String
substring references remain valid. Program-table bounds are checked, while
executable segments, relocations, ELF machine/entry semantics and the inner
firmware image are not loaded or interpreted.

`FirmwareImageView::buildIdNote` contains the entire `.note.gnu.build-id`
section, including its raw note header/name/padding. It does not extract the GNU
note descriptor, calculate a digest or authenticate an image. The signature is
also opaque. Upstream's
[signature allocation](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/nvidia/src/kernel/gpu/gsp/kernel_gsp.c#L5240)
rounds its real DMA buffer to 256-byte alignment for Booter; this parser neither
allocates that owner nor verifies signatures. The extracted image bytes can
feed the existing Radix3 planner/serializer after the caller obtains real DMA
pages. Container lifetime, release/chip selection, authentication, DMA owners,
cache synchronization and successful firmware boot remain caller/integration
requirements.

The firmware test constructs only synthetic ELF containers and opaque bytes.
It covers exact ordinary/CC selection, raw note representation, every shorter
container length, full-width offset/size overflow, malformed headers/strings,
missing or empty sections, duplicate names in separate string storage, overlaps,
NOBITS/NULL compatibility, version/terminal-NUL mismatches, resource budgets and
atomic output preservation. It acquires no actual firmware. A direct installed
compiler invocation is:

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -pedantic -I . \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  Drivers/PortedNvidiaGsp/FirmwareImage.cpp tests/ported_nvidia_gsp_firmware_test.cpp \
  -o /tmp/mellow-nvidia-gsp-firmware-test
/tmp/mellow-nvidia-gsp-firmware-test
```

Local verification on 2026-09-30 passed 2369 checks with Clang ASan/UBSan and
the same checks with installed GCC. `FirmwareImage.cpp` also compiled to
freestanding x86_64 Mach-O objects targeting macOS 15 and 26 using compiler
builtin headers with exceptions and RTTI disabled. These checks used no Apple
SDK, kext load, actual firmware, cryptographic authentication or physical GPU.

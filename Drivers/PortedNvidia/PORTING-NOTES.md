# NVIDIA command encoding partial port

This directory implements GPFIFO pushbuffer/control-NOP entry packing, explicit
little-endian serialization and DMA method headers for the eight inspected
channel formats. It is usable by a future submission adapter; it does not
publish a ring or execute a GPU command. Class identifiers describe formats,
not a detected device or a hardware support claim.

The immutable upstream revision is
`e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb`. Official MIT sources inspected:

- [Maxwell A class](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/common/sdk/nvidia/inc/class/clb06f.h)
- [Pascal A class](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/common/sdk/nvidia/inc/class/clc06f.h)
- [Volta A class](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/common/sdk/nvidia/inc/class/clc36f.h)
- [Turing A class](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/common/sdk/nvidia/inc/class/clc46f.h)
- [Ampere A class](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/common/sdk/nvidia/inc/class/clc56f.h)
- [Hopper A class](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/common/sdk/nvidia/inc/class/clc86f.h)
- [Blackwell A class](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/common/sdk/nvidia/inc/class/clc96f.h)
- [Blackwell B class](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/src/common/sdk/nvidia/inc/class/clca6f.h)
- [Turing UVM GPFIFO helpers](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/nvidia-uvm/uvm_turing_host.c)
- [UVM method packing macros](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/nvidia-uvm/uvm_push_macros.h)
- [Hopper extended-base helpers](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/nvidia-uvm/uvm_hopper_host.c)
- [UVM class/architecture inheritance](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/e4a5faa2567f28c8eabe0ebb6422b6d0abcf37eb/kernel-open/nvidia-uvm/uvm_hal.c)

The GPFIFO start address is byte-addressed and 4-byte aligned: entry 0 holds
address bits 31:2, entry 1 bits 7:0 hold address bits 39:32. Entry 1 bits 30:10
hold length in dwords; bit 31 selects proceed/wait. This implementation leaves
FETCH, LEVEL, and the USER/reserved bit clear, matching the inspected UVM helper.
The 40-bit limit belongs to this command field, not the device's full GPUVM.
Hopper/Blackwell add a control entry with address bits 56:40 in operand bits
24:8 and opcode 4. `encodeGpfifoSegment` returns the ordered control/fetch pair
and rejects a fetch that crosses a 40-bit segment. It explicitly emits a zero
base too. The public standalone fetch API rejects Hopper/Blackwell, so callers
must use the paired API even for low addresses; raw field packing is private.
Ada inherits Ampere architecture operations in the inspected UVM table; no
invented Ada class alias or per-device lookup is added. The caller must obtain
the actual negotiated channel class. Uninspected identifiers are rejected.

Method offsets use a 12-bit dword address, 3-bit subchannel, 13-bit count or
immediate value, and a 3-bit opcode. Header construction does not validate
engine methods or payload values. The checked range and no-wrap rules in
`provenance.json` are additional adapter policy, not new hardware discoveries.

The code is adapted from published fields and helper arithmetic, with original
MIT notices retained. Full upstream content hashes were not measured; no
upstream source tree, firmware or vendor binary was collected.

Host tests cover fixed numeric/byte vectors, independent field decoding,
representable boundaries, overflow, unsupported formats, and unchanged output
on errors. They establish packet construction only. XNU binding, GPUVM and
channel ownership, command admission, firmware, MMIO publication/order,
hardware fence/reset, user-space driver, Metal and display remain separate work.

Direct host build (run from the repository root):

```sh
c++ -std=c++17 -Wall -Wextra -Wconversion -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -I. Drivers/PortedNvidia/CommandEncoding.cpp tests/ported_nvidia_test.cpp -o build/ported-nvidia-test
build/ported-nvidia-test
```

# Repository layout

> **Scope and precedence:** [PLATFORM-DECISIONS](PLATFORM-DECISIONS.md),
> [PLATFORM-ARCHITECTURE](PLATFORM-ARCHITECTURE.md), and
> [IMPLEMENTATION-STATUS](IMPLEMENTATION-STATUS.md) govern conflicting draft assumptions.
> Runtime policy and source-intake tools are runnable; the Metal facade, shader JIT, live GL/CL
> providers and integrated XNU GPU backend are not implemented. No Mellow GPU/Metal PASS exists.

## 한글 요약

목표 디렉터리 구조와 현재→목표 이관 대응표다. **이 문서는 서술이며, P0에서는 실행하지 않는다.**
기존 source의 이관은 아직 실행하지 않았다. 새 Runtime/와 Tools/mellow_port/는 현재
존재하며, 아래 목표 kernel/userspace/tools 구조와 구별해야 한다.
핵심 원칙은 **평면별 분리** — `kernel/`(Plane 1–2), `userspace/`(Plane 3–4), `tools/`(Plane 0) —
와 **backend별 격리**다. 기존 증거 자료(`validation/`, `abi-evidence/`, `compiler-evidence/`,
`tests/*.json`)는 **전량 그대로 보존**한다.

---

**Status: current tree plus a proposed migration. New Runtime/policy and Tools/intake files exist.
Legacy source migration is not performed; target directories and future bundles below are proposals.**

## Current layout

```
Mellow/
├─ Mellow/                 retained legacy plug-in and Xe research modules
├─ Runtime/                PlatformRuntime.hpp/.cpp policy/cache contracts; no live provider
├─ Tools/                  build, ABI, Metal, test orchestration
│  ├─ mellow-port.py        current inspect/plan/generate entry point
│  ├─ mellow_port/          source analysis/report and bounded generation
│  └─ run-platform-tests.py policy contract host-test runner
├─ Userspace/              metal_session.py, mellow_acceptance.py
├─ tests/                  host tests and result JSON
├─ validation/             evidence JSON
├─ abi-evidence/           macOS ABI survey artifacts
├─ compiler-evidence/      Intel ocloc output
├─ docs/                   flat document set
├─ Lilu.kext/  MacKernelSDK/
└─ Mellow.xcodeproj/       existing native kext project; Runtime is not integrated into the kext
```

The flat `Mellow/` directory is the main problem: it mixes ~12k LOC of Apple-impersonation patching
with ~2.5k LOC of vendor-neutral-shaped Xe backend and the display code, with no boundary between
them.

## Proposed target layout — not the current filesystem

```
Mellow/
├─ README.md
├─ docs/
│  ├─ PLATFORM-ARCHITECTURE.md    reviewed architecture
│  ├─ PLATFORM-DECISIONS.md       reviewed contract decisions
│  ├─ IMPLEMENTATION-STATUS.md    current runnable code and evidence
│  ├─ CONCEPT.md                 concept draft
│  ├─ ARCHITECTURE.md            the five planes
│  ├─ EVIDENCE-POLICY.md         normative; everything references this
│  ├─ ROADMAP.md
│  ├─ REPO-LAYOUT.md             this file
│  ├─ LICENSING.md
│  ├─ LEGACY-DISPOSITION.md
│  ├─ GPU-SUPPORT-MATRIX.md
│  ├─ METAL-EMULATION.md         Plane 4
│  ├─ SHADER-JIT.md              Plane 4
│  ├─ AIR-ABI.md                 Plane 4
│  ├─ WORKLOAD-RUNTIME.md        Plane 3
│  ├─ MGAL.md                    Plane 2
│  ├─ MELLOW-UAPI.md             Plane 2
│  ├─ MELLOWKPI.md               Plane 1
│  ├─ BACKPORT-PIPELINE.md       Plane 0
│  ├─ ADDING-A-GPU.md            Plane 0 tutorial
│  ├─ METAL_FEATURE_MATRIX.md
│  ├─ PORTING_STATUS.md
│  ├─ backends/xe/               XE-*.md moved here
│  └─ history/                   superseded records, bodies unedited
├─ kernel/
│  ├─ MellowKMD/                 composition root, registry, UAPI, policy
│  ├─ mgal/                      the nine interfaces + pure logic
│  ├─ kpi/                       MellowKPI: linux/*, drm/*, ttm/*
│  ├─ backends/
│  │  ├─ xe/                     current Xe* refactored onto MGAL
│  │  ├─ applecompat/            ICL path only; TGL removed
│  │  ├─ amdgpu/                 generated + patches/
│  │  └─ nvidia-selected/        future reviewed RM or Nouveau adapter; not interchangeable
│  └─ display/                   DisplayMergeNub, HDMI, IntelDPLinkTraining
├─ userspace/
│  ├─ MellowMTL/                 core + interposition + plug-in adapters
│  ├─ MellowJIT/                 AIR → MIR → gated CL C / GLSL / IL / native adapters
│  ├─ MellowRT/                  router, resource model, IOSurface interop
│  ├─ MellowGL/  MellowCL/       Mesa-derived providers
│  └─ tools/                     metal_session.py, mellow_acceptance.py
├─ tools/
│  ├─ mellow-port/               the backport pipeline
│  └─ ...                        existing Tools/ scripts
├─ tests/                        host tests + conformance suites
├─ validation/                   evidence JSON — preserved in full
├─ abi-evidence/  compiler-evidence/
└─ Lilu.kext/  MacKernelSDK/  Mellow.xcodeproj/
```

Two organizing principles: **separation by plane**, and **isolation by backend** so that adding a
family-scoped changes are reviewable. Shared compiler, runtime, UAPI and firmware contracts may
still require changes; one directory does not guarantee isolation of semantics or licensing.

## Migration map

| Current | Proposed target, not performed | Notes |
| --- | --- | --- |
| `Mellow/kern_start.cpp`, `kern_mellow.*`, `kern_patcherplus.*`, `StartupPolicy.hpp`, `RuntimeReadiness.hpp`, `PatternMatch.hpp` | `kernel/MellowKMD/` | Entry, policy, readiness. Gains the composition root |
| `Mellow/HardwareAccess.hpp` | `kernel/mgal/` | Already OS-independent — the model for the rest |
| `Mellow/kern_model.hpp` | `kernel/mgal/` | Becomes the device descriptor table for `IMellowDevice` |
| `Mellow/Xe*.cpp/.hpp` (20 + 24 files) | `kernel/backends/xe/` | Refactored onto MGAL interfaces per [MGAL.md](MGAL.md) |
| `Mellow/kern_genx.*`, ICL routes from `kern_gen11.*` | `kernel/backends/applecompat/` | See [LEGACY-DISPOSITION.md](LEGACY-DISPOSITION.md) |
| `Mellow/AppleIntelParams.hpp`, TGL routes, `MellowDriverProfiles.TGL` | Quarantined legacy research | No deletion in this redesign; review later changes separately |
| `Mellow/DisplayMergeNub.*`, `HDMI.*`, `IntelDPLinkTraining.*` | `kernel/display/` | Kept and promoted |
| `Mellow/DYLDPatches.*` | `userspace/MellowMTL/` (attachment) | Minus ~210 lines of unreferenced AMD VCN tables; any future hook activation requires a version-pinned ABI experiment |
| `Mellow/Info.plist` | `kernel/MellowKMD/Info.plist` | Personalities revised; profiles generalized per backend |
| `Userspace/*.py` | `userspace/tools/` | Unchanged content |
| `Tools/*` | `tools/` | Future migration only; update imports/entry points and tests first |
| `Runtime/PlatformRuntime.*` | `userspace/MellowRT/` policy layer | Existing code; destination proposed, live execution unimplemented |
| `docs/XE-*.md` | `docs/backends/xe/` | Bodies unedited |
| `docs/IOACCEL-METAL.md`, `METAL-IMPLEMENTATION-PLAN.md`, `METAL-USERSPACE.md`, `METAL-PATH-CHANGES.md`, `USERLAND-METAL-EVIDENCE-AUDIT-*.md`, `NATIVE-XE-BACKEND-AUDIT.md`, `DRIVER-CORE-CHANGES.md`, `STARTUP-PATCHER-CHANGES.md`, `ACCEPTANCE-0.4.1.ko.md`, `BUILD-VALIDATION.md`, `EXPERIMENTS.md`, `UPSTREAM-README.md` | `docs/history/` | **Bodies unedited.** Banners added in P0 |
| `validation/`, `abi-evidence/`, `compiler-evidence/`, `tests/*.json` | unchanged | **Preserved in full** |

## Preservation rules

1. **No evidence artifact is deleted or edited.** Every JSON in `validation/`, `abi-evidence/`,
   `compiler-evidence/`, and `tests/` stays exactly as it is. Their negative-claim fields are the
   machine-readable form of this project's honesty and they are not rewritten to match new
   ambitions.
2. **Historical documents keep their bodies.** A superseded document gets a banner and a move,
   never an edit. Rewriting an old assessment to agree with a new plan destroys the record of what
   was actually known when.
3. **Provenance manifests are append-only.** Existing entries are corrected only for the pinning
   defect described in [LICENSING.md](LICENSING.md).

## Build implications

Two facts make the reorganization safer than it looks:

- [Tools/cross-build.py](../Tools/cross-build.py) **parses `PBXSourcesBuildPhase` out of the
  pbxproj** and resolves each name against a search path, so the Xcode and command-line builds
  cannot silently diverge. Adding directories means updating the resolver's search paths, not
  duplicating a file list.
- The project already hashes every input before and after a build and refuses the artifact if
  anything changed, which makes a before-and-after comparison across the move meaningful.

The Xcode project keeps one kext target during P1. Splitting into `MellowKMD` plus per-backend
kexts — linked through `OSBundleLibraries` on `com.NiSeullent.MellowKMD` — happens in P2, when
there is a composition root for them to attach to.

## Existing and proposed bundle identifiers

| Bundle | Identifier | Phase |
| --- | --- | --- |
| Legacy plug-in | `com.NiSeullent.Mellow` | exists |
| Core kext | `com.NiSeullent.MellowKMD` | P2 |
| Backend modules | `com.NiSeullent.MellowKMD.<backend>` | P3+ |
| Metal driver bundle | `com.NiSeullent.MellowMTLDriver` | P5+ |

#!/usr/bin/env python3
"""Build and run portable driver contract tests; never loads a GPU driver."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('g++') or shutil.which('clang++'))
    parser.add_argument('--out', type=Path, required=True, help='Scratch build/report directory')
    args = parser.parse_args()
    if not args.cxx:
        parser.error('Specify --cxx with an installed C++ compiler')
    root = Path(__file__).resolve().parent.parent
    args.out.mkdir(parents=True, exist_ok=True)
    compiler = subprocess.run([args.cxx, '--version'], capture_output=True, text=True, check=True)
    results = []
    # accel_contracts_harness.cpp requires production function extraction and
    # has its own runner; it must not compile as an independent test program.
    suites = [
        ('tests/HardwareAccessTests.cpp', [], []),
        ('tests/modelTests.cpp', [], []),
        ('tests/patcher_policy_tests.cpp', [], []),
        ('tests/xe_guc_tests.cpp', ['Mellow/XeGuCTransport.cpp'], [root / 'Mellow']),
        ('tests/ported_nvidia_test.cpp', ['Drivers/PortedNvidia/CommandEncoding.cpp'], [root]),
        ('Userspace/WindowServer/SurfaceSnapshotTests.cpp',
         ['Userspace/WindowServer/SurfaceSnapshot.cpp'], [root]),
        ('Userspace/WindowServer/RenderFixtureOracleTests.cpp', [], [root]),
        ('tests/opencl_runtime_regression.cpp',
         ['Runtime/PlatformRuntime.cpp', 'Runtime/OpenCLProvider.cpp'], [root],
         ['-DMELLOW_OPENCL_TESTING'], [] if sys.platform in ('win32', 'darwin') else ['-ldl']),
        ('tests/xe_context_execution_tests.cpp',
         ['Mellow/XeContextExecution.cpp', 'Mellow/XeContext.cpp', 'Mellow/XeDispatch.cpp',
          'Mellow/XeMemory.cpp', 'Mellow/XeZebin.cpp', 'Mellow/XeGuCTransport.cpp',
          'Mellow/XeFence.cpp', 'Drivers/PortedXe/XePageTable.cpp'], [root / 'Mellow'],
         [], [], ['compiler-evidence/mellow_evidence_mtl.bin']),
    ]
    for suite in suites:
        name, production_inputs, include_dirs = suite[:3]
        compile_flags = suite[3] if len(suite) > 3 else []
        link_flags = suite[4] if len(suite) > 4 else []
        runtime_inputs = suite[5] if len(suite) > 5 else []
        source = root / name
        inputs = [root / path for path in production_inputs] + [source]
        compile_inputs = [path.relative_to(root).as_posix() for path in inputs]
        source_sha256 = {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in inputs}
        binary = args.out / (source.stem + ('.exe' if sys.platform == 'win32' else ''))
        command = [args.cxx, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   *compile_flags, *['-I' + str(path) for path in include_dirs],
                   *map(str, inputs), *link_flags, '-o', str(binary)]
        compile_result = subprocess.run(command, capture_output=True, text=True)
        test = dict(source=source.relative_to(root).as_posix(), compile_exit=compile_result.returncode,
                    compile_output=compile_result.stdout + compile_result.stderr,
                    production_inputs=production_inputs, compile_inputs=compile_inputs,
                    compile_flags=compile_flags, link_flags=link_flags,
                    runtime_inputs=runtime_inputs,
                    runtime_input_sha256={name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                                          for name in runtime_inputs},
                    source_sha256=source_sha256, compile_command=command)
        if compile_result.returncode == 0:
            try:
                run = subprocess.run([str(binary.resolve()), *[str(root / name) for name in runtime_inputs]],
                                     cwd=root, capture_output=True, text=True, timeout=30)
                test.update(run_exit=run.returncode, output=run.stdout + run.stderr)
            except subprocess.TimeoutExpired:
                test.update(run_exit=None, output='TIMEOUT after 30 seconds')
        results.append(test)
        print(test['source'], 'PASS' if test.get('run_exit') == 0 else 'FAIL')
        if test.get('output'): print(test['output'].strip())
        if compile_result.returncode: print(test['compile_output'])
    passed = bool(results) and all(x.get('run_exit') == 0 for x in results)
    report = dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        host=platform.platform(), compiler=compiler.stdout.splitlines()[0],
        scope='Portable driver helpers and contracts; no IOKit loader, firmware, GPU or Metal execution',
        source_hash_scope='Explicit compile inputs and runtime data inputs; included headers and transitive dependencies are not hashed',
        hardware_execution=False, passed=passed, results=results)
    (args.out / 'host-tests.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    return 0 if passed else 1

if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Build production GGTT manager with ASan/UBSan; no hardware access."""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('clang++') or shutil.which('g++'))
    args = parser.parse_args()
    if not args.cxx:
        parser.error('C++ compiler required')
    spec = importlib.util.spec_from_file_location('ported_tests', ROOT / 'Tools/run-ported-xe-tests.py')
    port = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(port)
    print(port.verify_provenance(), flush=True)
    with tempfile.TemporaryDirectory(prefix='xe-ggtt-') as scratch:
        binary = Path(scratch) / 'ggtt-tests'
        command = [args.cxx, '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-pedantic', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   str(ROOT / 'Mellow/XeGgtt.cpp'), str(ROOT / 'Drivers/PortedXe/XePageTable.cpp'),
                   str(ROOT / 'tests/xe_ggtt_tests.cpp'), '-o', str(binary)]
        print(' '.join(command), flush=True)
        subprocess.run(command, check=True, timeout=120)
        env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        subprocess.run([str(binary)], check=True, env=env, timeout=60)
    return 0

if __name__ == '__main__':
    raise SystemExit(main())

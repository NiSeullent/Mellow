#!/bin/sh
# Collect only local diagnostic evidence. Never uploads, installs or changes firmware.
set -eu
if [ "$(uname -s)" != Darwin ]; then echo 'Run this on the target macOS installation.' >&2; exit 2; fi
if [ "$#" -ne 1 ]; then echo 'Usage: collect-255u-logs.sh NEW_PRIVATE_DIRECTORY' >&2; exit 2; fi
out=$1
if [ -e "$out" ]; then echo 'Use a new directory; existing evidence is not overwritten.' >&2; exit 2; fi
umask 077
mkdir -p -- "$out"
sw_vers > "$out/os-version.txt"
uname -r > "$out/darwin-version.txt"
ioreg -r -c MellowTahoeDiagnostic -l > "$out/mellow-diagnostic-ioreg.txt" 2> "$out/ioreg-errors.txt" || true
kmutil showloaded 2> "$out/kmutil-errors.txt" | grep -E 'Mellow|Lilu|VirtualSMC|PS2|RealtekRTL8111|NVMeFix' > "$out/loaded-relevant-kexts.txt" || true
log show --last boot --style compact --predicate 'process == "kernel" AND eventMessage CONTAINS[c] "mellow"' > "$out/mellow-kernel-log.txt" 2> "$out/log-errors.txt" || true
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ "$(id -u)" -eq 0 ] && [ -x "$here/sequoia-probe" ]; then
    "$here/sequoia-probe" > "$out/physical-probe.json" 2> "$out/physical-probe-errors.txt" || echo 'Probe unavailable; inspect errors, do not treat this as success.' > "$out/probe-unavailable.txt"
else
    echo 'Administrator-only physical probe was not run.' > "$out/probe-unavailable.txt"
fi
printf '%s\n' 'Private local evidence. Review for identifiers before sharing. Nothing was uploaded.' 'A loaded diagnostic service or physical probe is not Metal acceleration.' > "$out/READ-ME.txt"
printf 'Created private evidence directory: %s\n' "$out"

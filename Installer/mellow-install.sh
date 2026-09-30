#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mellow contributors. See repository LICENSE and NOTICE.
# Download and review this file before running it. No curl | bash is required.
set -euo pipefail
export LC_ALL=C
usage() {
    cat <<'EOF'
Mellow native Intel macOS development installer bootstrap
Usage: bash ./mellow-install.sh --release TAG [--prefix ABSOLUTE_PATH]
       bash ./mellow-install.sh --help

Downloads the exact native CLI from NiSeullent/Mellow, verifies the GitHub
release asset SHA256 digest, then invokes its per-user installation command.
The CLI verifies the actual development kext/framework package independently.
No sudo, kext activation, EFI/config.plist edit, SIP change or reboot is used.
System Metal and WindowServer acceleration are not registered or verified.
EOF
}
if [ "$#" -eq 0 ] || { [ "$#" -eq 1 ] && [ "$1" = "--help" ]; }; then usage; exit 0; fi
fail() { printf 'Mellow: %s\n' "$1" >&2; exit 1; }
release=''; prefix=''
while [ "$#" -gt 0 ]; do
    case "$1" in
        --release) [ "$#" -ge 2 ] && [ -z "$release" ] || fail 'Missing or repeated --release'; release="$2"; shift 2 ;;
        --prefix) [ "$#" -ge 2 ] && [ -z "$prefix" ] || fail 'Missing or repeated --prefix'; prefix="$2"; shift 2 ;;
        *) fail 'Unknown option; use --help' ;;
    esac
done
[[ "$release" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] && [ "${#release}" -le 96 ] || fail 'Invalid release tag'
[ -z "$prefix" ] || [[ "$prefix" == /* ]] || fail 'Prefix must be an absolute path'
[ "$(/usr/bin/uname -s)" = Darwin ] && [ "$(/usr/bin/uname -m)" = x86_64 ] || fail 'Only native Intel macOS is supported'
[ "$(/usr/bin/id -u)" -ne 0 ] || fail 'Run as your normal user without sudo'
translated_error=''
if translated=$(/usr/sbin/sysctl -n sysctl.proc_translated 2>&1); then
    [ "$translated" = 0 ] || fail 'Rosetta execution is unsupported'
else
    translated_error="$translated"
    [[ "$translated_error" == *"unknown oid"* ]] || fail 'Cannot determine native host translation state'
fi
version=$(/usr/bin/sw_vers -productVersion); major=${version%%.*}
case "$major" in 15|26) ;; *) fail 'Only macOS 15 and 26 are supported' ;; esac
temporary=$(/usr/bin/mktemp -d "${TMPDIR:-/tmp}/mellow-bootstrap.XXXXXX")
trap '/bin/rm -rf "$temporary"' EXIT
/bin/chmod 700 "$temporary"
curl_https() {
    /usr/bin/curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
        --max-redirs 4 --connect-timeout 20 --max-time 300 "$@"
}
metadata="$temporary/release.json"
final=$(curl_https --max-filesize 1048576 --header 'Accept: application/vnd.github+json' \
    --header 'X-GitHub-Api-Version: 2022-11-28' --output "$metadata" --write-out '%{url_effective}' \
    "https://api.github.com/repos/NiSeullent/Mellow/releases/tags/$release")
[ "$final" = "https://api.github.com/repos/NiSeullent/Mellow/releases/tags/$release" ] || fail 'Unexpected API redirect'
name="mellow-installer-macos${major}-x86_64"
/usr/bin/osascript -l JavaScript -e '
ObjC.import("Foundation");
function run(args) {
  var raw = $.NSString.stringWithContentsOfFileEncodingError(args[0], $.NSUTF8StringEncoding, null);
  if (!raw) throw Error("Cannot read release metadata");
  var release = JSON.parse(raw.js), tag = args[1], name = args[2];
  if (release.tag_name !== tag || release.draft !== false || !Array.isArray(release.assets)) throw Error("Wrong release identity");
  var matches = release.assets.filter(function (asset) { return asset.name === name; });
  if (matches.length !== 1) throw Error("Native CLI asset absent or duplicated");
  var asset = matches[0], url = "https://github.com/NiSeullent/Mellow/releases/download/" + tag + "/" + name;
  if (asset.state !== "uploaded" || asset.browser_download_url !== url || !/^sha256:[0-9a-f]{64}$/.test(asset.digest || "") ||
      !Number.isSafeInteger(asset.size) || asset.size <= 0 || asset.size > 33554432) throw Error("Invalid CLI asset URL, digest or size");
  return url + "\n" + asset.digest.substring(7);
}' "$metadata" "$release" "$name" > "$temporary/selection"
{ IFS= read -r address; IFS= read -r expected; } < "$temporary/selection"
[ "$address" = "https://github.com/NiSeullent/Mellow/releases/download/$release/$name" ] && [[ "$expected" =~ ^[0-9a-f]{64}$ ]] || fail 'Unexpected native asset selection'
binary="$temporary/$name"
final=$(curl_https --max-filesize 33554432 --output "$binary" --write-out '%{url_effective}' "$address")
case "$final" in
    https://github.com/NiSeullent/Mellow/releases/download/*|https://release-assets.githubusercontent.com/*|https://objects.githubusercontent.com/*) ;;
    *) fail 'Unexpected GitHub asset redirect' ;;
esac
actual=$(/usr/bin/shasum -a 256 "$binary"); actual=${actual%% *}
[ "$actual" = "$expected" ] || fail 'Native CLI SHA256 mismatch; downloaded code was not executed'
/bin/chmod 700 "$binary"
if [ -n "$prefix" ]; then "$binary" install --release "$release" --prefix "$prefix";
else "$binary" install --release "$release"; fi

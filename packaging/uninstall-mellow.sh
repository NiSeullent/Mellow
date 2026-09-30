#!/bin/bash
# Remove only recorded files. Never unload a live kext or change security policy.
set -e
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export LC_ALL=C
umask 077
prefix="${HOME}/Library/Mellow"
remove_kext=0 remove_dependencies=0 prepare_kext=0
temporary= lock= system_locked=0 preserved=0

usage() {
    cat <<'USAGE'
Usage: bash uninstall-mellow.sh [OPTIONS]
  --prefix ABSOLUTE_PATH     User-space prefix; default: ~/Library/Mellow.
  --remove-kext              Also remove Mellow.kext installed by this installer.
  --remove-dependencies      With --remove-kext, remove installer-owned Lilu too.
  --prepare-kext             With --remove-kext, ask kmutil to check/rebuild caches.
  --help                    Show help without changing anything.

Only recorded, unchanged files are removed. Modified and unregistered files are
preserved. Other installers' kexts/Lilu are never removed. Loaded kernel code
is not unloaded; a restart and normal macOS approval/cache policy may apply.
Run as your normal user. Only explicit kernel removal asks sudo for access.
USAGE
}
die() { printf 'Mellow: %s\n' "$*" >&2; exit 2; }
hash_file() { /usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'; }
field() { /usr/bin/plutil -extract "$2" raw -expect "$3" -o - "$1"; }
valid_relative() {
    local name=$1 part parts=()
    [[ "$name" =~ ^[A-Za-z0-9_.@+/-]+$ ]] || return 1
    case "$name" in /*|*//*|*/|.) return 1;; esac
    IFS=/ read -r -a parts <<< "$name"
    for part in "${parts[@]}"; do [ -n "$part" ] && [ "$part" != . ] && [ "$part" != .. ] || return 1; done
}
safe_absolute() {
    local path=$1 part current= parts=()
    case "$path" in /*) ;; *) return 1;; esac
    case "$path" in /|*//*|*$'\n'*|*$'\r'*|*$'\t'*) return 1;; esac
    IFS=/ read -r -a parts <<< "$path"
    for part in "${parts[@]}"; do
        [ -n "$part" ] || continue
        [ "$part" != . ] && [ "$part" != .. ] || return 1
        current="$current/$part"
        [ ! -L "$current" ] || return 1
    done
}
checksum_line() {
    local line=$1
    [[ "$line" =~ ^([a-f0-9]{64})\ \ ([A-Za-z0-9_.@+/-]+)$ ]] || return 1
    line_sha=${BASH_REMATCH[1]} line_name=${BASH_REMATCH[2]}
    valid_relative "$line_name"
}
validate_registry() {
    local registry=$1 kind=$2 line
    [ -f "$registry" ] && [ ! -L "$registry" ] &&
        [ "$(/usr/bin/stat -f %z "$registry")" -le 16777216 ] || die 'Invalid installation registry.'
    : > "$temporary/registry-names"
    while IFS= read -r line || [ -n "$line" ]; do
        checksum_line "$line" || die 'Malformed installation registry.'
        if [ "$kind" = user ]; then
            case "$line_name" in manifest.json|SHA256SUMS|bin/*|Frameworks/*|licenses/*) ;; *) die 'Registry names a non-package user file.';; esac
        else
            case "$line_name" in Contents/*) ;; *) die 'Registry names a non-bundle file.';; esac
        fi
        printf '%s\n' "$line_name" >> "$temporary/registry-names"
    done < "$registry"
    [ -s "$temporary/registry-names" ] || die 'Empty installation registry.'
    /usr/bin/sort "$temporary/registry-names" | /usr/bin/uniq -d > "$temporary/duplicates"
    [ ! -s "$temporary/duplicates" ] || die 'Duplicate registry path.'
}
record_directories() {
    local name=$1 directory=${1%/*}
    while [ "$directory" != "$name" ]; do
        printf '%s\n' "$directory" >> "$temporary/directories"
        name=$directory directory=${directory%/*}
    done
}
remove_empty_directories() {
    local root=$1 privileged=$2 directory
    /usr/bin/sort -ru "$temporary/directories" > "$temporary/directories.sorted"
    while IFS= read -r directory; do
        # Registered files may have been replaced by symlinks. Preserve their
        # ancestor tree instead of traversing it to remove external directories.
        safe_absolute "$root/$directory" || continue
        if [ "$privileged" -eq 1 ]; then /usr/bin/sudo -n /bin/rmdir "$root/$directory" 2>/dev/null || true;
        else /bin/rmdir "$root/$directory" 2>/dev/null || true; fi
    done < "$temporary/directories.sorted"
}
cleanup() {
    local result=$?
    trap - EXIT HUP INT TERM
    set +e
    [ "$system_locked" -eq 0 ] || /usr/bin/sudo -n /bin/rmdir /Library/Extensions/.Mellow-installer.lock
    [ -z "$lock" ] || /bin/rmdir "$lock" 2>/dev/null
    [ -z "$temporary" ] || /bin/rm -rf "$temporary"
    exit "$result"
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --help|-h) usage; exit 0;;
        --prefix) [ "$#" -ge 2 ] && [ -n "$2" ] || die 'Missing --prefix value.'; prefix=$2; shift 2;;
        --remove-kext) remove_kext=1; shift;;
        --remove-dependencies) remove_dependencies=1; shift;;
        --prepare-kext) prepare_kext=1; shift;;
        *) die "Unknown option: $1";;
    esac
done
[ "$(/usr/bin/uname -s)" = Darwin ] || die 'Run this uninstaller on macOS.'
[ "$(/usr/bin/id -u)" != 0 ] || die 'Run as your normal user, not sudo bash.'
prefix=${prefix%/}
safe_absolute "$prefix" || die 'Prefix must be absolute, non-root and have no symlink ancestors or dot components.'
case "$prefix" in /System|/System/*|/Library/Extensions|/Library/Extensions/*) die 'Reserved system prefix.';; esac
[ "$remove_dependencies" -eq 0 ] || [ "$remove_kext" -eq 1 ] || die '--remove-dependencies requires --remove-kext.'
[ "$prepare_kext" -eq 0 ] || [ "$remove_kext" -eq 1 ] || die '--prepare-kext requires --remove-kext.'
temporary=$(/usr/bin/mktemp -d /private/tmp/mellow-uninstall.XXXXXXXX)
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 129' HUP
trap 'exit 143' TERM
if [ -d "$prefix" ]; then
    lock="$prefix.mellow-lock"
    /bin/mkdir "$lock" || { lock=; die 'Prefix lock is held; inspect a leftover lock manually.'; }
    [ -f "$prefix/.mellow-files" ] && [ ! -L "$prefix/.mellow-files" ] || die 'Prefix was not installed by this installer; nothing removed.'
    IFS= read -r magic < "$prefix/.mellow-files"
    [ "$magic" = 'Mellow userspace receipt v1' ] || die 'Unknown installation receipt.'
    /usr/bin/tail -n +2 "$prefix/.mellow-files" > "$temporary/user-files"
    validate_registry "$temporary/user-files" user
    : > "$temporary/directories"
    user_changed=0
    while IFS= read -r line || [ -n "$line" ]; do
        checksum_line "$line" || die 'Invalid user registry.'
        path="$prefix/$line_name"
        if [ -e "$path" ] || [ -L "$path" ]; then
            if safe_absolute "$path" && [ -f "$path" ] && [ "$(hash_file "$path")" = "$line_sha" ]; then
                /bin/rm "$path"
            else printf 'Preserved modified file: %s\n' "$path" >&2; user_changed=1; preserved=1; fi
        fi
        record_directories "$line_name"
    done < "$temporary/user-files"
    remove_empty_directories "$prefix" 0
    if [ "$user_changed" -eq 0 ]; then /bin/rm "$prefix/.mellow-files"; fi
    /bin/rmdir "$prefix" 2>/dev/null || printf 'Remaining files/directories preserved at %s\n' "$prefix"
    printf 'Recorded user-space files removed; no system driver was unloaded.\n'
elif [ -e "$prefix" ]; then die 'Prefix is not a directory.';
else printf 'No user-space installation at %s\n' "$prefix"; fi

remove_bundle() {
    local name=$1 identifier=$2 line path relative
    local target="/Library/Extensions/$name.kext" receipt="/Library/Extensions/.Mellow-installer-$name.receipt"
    if [ ! -e "$target" ] && [ ! -e "$receipt" ]; then printf '%s is absent.\n' "$name"; return 0; fi
    [ -d "$target" ] && [ -f "$receipt" ] && safe_absolute "$target" && [ ! -L "$receipt" ] ||
        die "$name is not managed by this installer; it was not removed."
    { IFS= read -r magic; IFS= read -r recorded_id; } < "$receipt"
    [ "$magic" = 'Mellow managed bundle v1' ] && [ "$recorded_id" = "$identifier" ] || die "Unknown $name receipt."
    safe_absolute "$target/Contents/Info.plist" &&
        [ "$(field "$target/Contents/Info.plist" CFBundleIdentifier string)" = "$identifier" ] || die "$name has a different or incomplete bundle identity; it was preserved."
    local registry="$temporary/$name-files"
    /usr/bin/tail -n +3 "$receipt" > "$registry"
    validate_registry "$registry" bundle
    # A stale or incomplete receipt must not authorize a partial kernel-bundle
    # deletion. Require exact coverage before changing any of its files.
    /usr/bin/find "$target" ! -type d -print0 > "$temporary/$name-existing"
    : > "$temporary/$name-actual"
    while IFS= read -r -d '' path; do
        relative=${path#"$target/"}
        valid_relative "$relative" || die "$name contains an unsafe path; it was preserved."
        printf '%s\n' "$relative" >> "$temporary/$name-actual"
    done < "$temporary/$name-existing"
    /usr/bin/sort "$temporary/registry-names" > "$temporary/$name-expected.sorted"
    /usr/bin/sort "$temporary/$name-actual" > "$temporary/$name-actual.sorted"
    /usr/bin/cmp -s "$temporary/$name-expected.sorted" "$temporary/$name-actual.sorted" || die "$name receipt does not cover exactly its files; bundle and receipt were preserved."
    # Validate the complete inverse before removing any part of a kernel bundle.
    while IFS= read -r line || [ -n "$line" ]; do
        checksum_line "$line" || die 'Invalid bundle registry.'
        path="$target/$line_name"
        if [ -e "$path" ] || [ -L "$path" ]; then
            safe_absolute "$path" && [ -f "$path" ] && [ "$(hash_file "$path")" = "$line_sha" ] ||
                die "$name was modified; all its files were preserved."
        fi
    done < "$registry"
    : > "$temporary/directories"
    while IFS= read -r line || [ -n "$line" ]; do
        checksum_line "$line" || die 'Invalid bundle registry.'
        path="$target/$line_name"
        if [ -f "$path" ]; then /usr/bin/sudo -n /bin/rm "$path"; fi
        record_directories "$line_name"
    done < "$registry"
    remove_empty_directories "$target" 1
    if ! /usr/bin/sudo -n /bin/rmdir "$target" 2>/dev/null; then
        printf 'Bundle remnants and receipt preserved at %s; inspect before further removal.\n' "$target" >&2
        preserved=1
        return 0
    fi
    /usr/bin/sudo -n /bin/rm "$receipt"
    printf 'Removed recorded %s files from disk; any loaded code remains until normal OS shutdown/restart.\n' "$name"
}
if [ "$remove_kext" -eq 1 ]; then
    safe_absolute /Library/Extensions && [ -d /Library/Extensions ] &&
        [ "$(/usr/bin/stat -f %u /Library/Extensions)" = 0 ] || die 'Unsafe or unavailable system extension directory.'
    /usr/bin/sudo -v
    /usr/bin/sudo -n /bin/mkdir /Library/Extensions/.Mellow-installer.lock || die 'Kernel installation lock is held.'
    system_locked=1
    remove_bundle Mellow com.NiSeullent.Mellow
    if [ "$remove_dependencies" -eq 1 ]; then
        [ ! -e /Library/Extensions/Mellow.kext ] || die 'Mellow bundle remnants remain; Lilu was preserved.'
        printf 'Explicitly removing installer-owned Lilu; other extensions may also need it.\n'
        remove_bundle Lilu as.vit9696.Lilu
    fi
fi
if [ "$prepare_kext" -eq 1 ]; then
    if ! /usr/bin/sudo -n /usr/bin/kmutil install --volume-root / --check-rebuild; then
        printf 'File removal completed, but macOS kernel-cache preparation failed. No security setting was changed.\n' >&2
        exit 3
    fi
fi
[ "$preserved" -eq 0 ] || exit 3

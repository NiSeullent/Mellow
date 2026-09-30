#!/bin/bash
# macOS system tools only. No GPU clients, kext loading or security changes.
set -e
set -o pipefail
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export LC_ALL=C
umask 077

ASSET=Mellow-macos-x86_64.zip
REPOSITORY=NiSeullent/Mellow
source_kind= source_value= expected_sha= checksum_file=
prefix="${HOME}/Library/Mellow"
install_kext=0 install_dependencies=0 prepare_kext=0
temporary= stage= previous= lock= system_stage=
user_published=0 committed=0 system_locked=0
system_targets=() system_receipts=() system_new=() system_old=()
system_new_receipt=() system_old_receipt=()

usage() {
    cat <<'USAGE'
Usage: bash install-mellow.sh SOURCE [OPTIONS]
  --version TAG, --tag TAG    Download the named GitHub release (prereleases work).
  --archive ZIP              Use an existing local release archive.
  --url HTTPS_URL            Download a release archive from an explicit HTTPS URL.
  --sha256 HEX               Independently supplied SHA-256 of the ZIP.
  --checksum-file FILE       External checksum file containing the ZIP's SHA-256.
  --prefix ABSOLUTE_PATH     User-space destination; default: ~/Library/Mellow.
  --install-kext             Also copy Mellow.kext to /Library/Extensions.
  --install-dependencies     With --install-kext, install bundled Lilu if absent.
  --prepare-kext             With --install-kext, ask kmutil to check/rebuild caches.
  --help                    Show this help without changing anything.

Default installation includes Frameworks, CLI tools and licenses only. Nothing
is run, loaded, registered with Metal, or added to your shell configuration.
The current release is experimental, lacks Developer ID signing/notarization,
and has no verified native GPU/Metal/WindowServer support. Kernel installation
may be refused by normal macOS security policy; this script never weakens it.
Run as your normal user. Only explicit kernel operations ask sudo for access.
USAGE
}
die() { printf 'Mellow: %s\n' "$*" >&2; exit 2; }
argument() { [ "$#" -ge 2 ] && [ -n "$2" ] || die "Missing value for $1"; }
choose_source() {
    [ -z "$source_kind" ] || die 'Choose exactly one of --archive, --version or --url.'
    source_kind=$1 source_value=$2
}
valid_hash() { [[ "$1" =~ ^[[:xdigit:]]{64}$ ]]; }
hash_file() { /usr/bin/shasum -a 256 "$1" | /usr/bin/awk '{print $1}'; }
field() { /usr/bin/plutil -extract "$2" raw -expect "$3" -o - "$1"; }
valid_relative() {
    local name=$1 piece
    [[ "$name" =~ ^[A-Za-z0-9_.@+/-]+$ ]] || return 1
    case "$name" in /*|*//*|*/|.) return 1;; esac
    local parts=()
    IFS=/ read -r -a parts <<< "$name"
    for piece in "${parts[@]}"; do
        [ "$piece" != . ] && [ "$piece" != .. ] && [ -n "$piece" ] || return 1
    done
}
user_relative() {
    valid_relative "$1" || return 1
    case "$1" in manifest.json|SHA256SUMS|bin/*|Frameworks/*|licenses/*) return 0;; esac
    return 1
}
safe_absolute() {
    local path=$1 current= piece
    case "$path" in /*) ;; *) return 1;; esac
    case "$path" in /|*//*|*$'\n'*|*$'\r'*|*$'\t'*) return 1;; esac
    local parts=()
    IFS=/ read -r -a parts <<< "$path"
    for piece in "${parts[@]}"; do
        [ -n "$piece" ] || continue
        [ "$piece" != . ] && [ "$piece" != .. ] || return 1
        current="$current/$piece"
        [ ! -L "$current" ] || return 1
    done
}
version_at_least() {
    local have=$1 need=$2 i a=() b=()
    [[ "$have" =~ ^[0-9]+(\.[0-9]+){0,3}$ ]] || return 1
    [[ "$need" =~ ^[0-9]+(\.[0-9]+){0,3}$ ]] || return 1
    IFS=. read -r -a a <<< "$have"
    IFS=. read -r -a b <<< "$need"
    for ((i=0;i<4;i++)); do
        local x=${a[$i]:-0} y=${b[$i]:-0}
        [ "${#x}" -le 5 ] && [ "${#y}" -le 5 ] || return 1
        ((10#$x > 10#$y)) && return 0
        ((10#$x < 10#$y)) && return 1
    done
    return 0
}
checksum_line() {
    local line=$1
    [[ "$line" =~ ^([a-f0-9]{64})\ \ ([A-Za-z0-9_.@+/-]+)$ ]] || return 1
    line_sha=${BASH_REMATCH[1]} line_name=${BASH_REMATCH[2]}
    valid_relative "$line_name"
}
# Only registered, unchanged files are removed, including during rollback.
# Empty directories are derived from these names; arbitrary prefix trees are
# never recursively deleted. Private mktemp staging trees are separate below.
remove_registered() {
    local root=$1 registry=$2 line path directory complete=1
    local directories="$temporary/remove-directories"
    : > "$directories"
    while IFS= read -r line || [ -n "$line" ]; do
        checksum_line "$line" && user_relative "$line_name" || return 1
        path="$root/$line_name"
        if [ -e "$path" ] || [ -L "$path" ]; then
            if safe_absolute "$path" && [ -f "$path" ] &&
                [ "$(hash_file "$path")" = "$line_sha" ]; then
                /bin/rm "$path" || complete=0
            else
                printf 'Preserved changed file: %s\n' "$path" >&2
                complete=0
            fi
        fi
        directory=${line_name%/*}
        while [ "$directory" != "$line_name" ]; do
            printf '%s\n' "$directory" >> "$directories"
            line_name=$directory directory=${directory%/*}
        done
    done < "$registry"
    /usr/bin/sort -ru "$directories" > "$directories.sorted"
    while IFS= read -r directory; do
        # A changed package path may now have a symlink ancestor. Do not let
        # directory cleanup follow that ancestor into an unrelated tree.
        safe_absolute "$root/$directory" || continue
        /bin/rmdir "$root/$directory" 2>/dev/null || true
    done < "$directories.sorted"
    if [ "$complete" -eq 1 ]; then /bin/rm -f "$root/.mellow-files"; fi
    /bin/rmdir "$root" 2>/dev/null || return 1
    [ "$complete" -eq 1 ]
}
cleanup() {
    local result=$? i old_paths
    trap - EXIT HUP INT TERM
    set +e
    if [ "$committed" -eq 0 ]; then
        for ((i=${#system_targets[@]}-1;i>=0;i--)); do
            if [ "${system_new_receipt[$i]:-0}" -eq 1 ]; then
                /usr/bin/sudo -n /bin/mv "${system_receipts[$i]}" "$system_stage/new-receipt-$i"
            fi
            if [ "${system_new[$i]:-0}" -eq 1 ]; then
                /usr/bin/sudo -n /bin/mv "${system_targets[$i]}" "$system_stage/new-$i.kext"
            fi
            if [ "${system_old[$i]:-0}" -eq 1 ]; then
                if [ ! -e "${system_targets[$i]}" ] && [ ! -L "${system_targets[$i]}" ]; then
                    /usr/bin/sudo -n /bin/mv "$system_stage/old-$i.kext" "${system_targets[$i]}" ||
                        printf 'Prior kernel bundle preserved at %s/old-%s.kext\n' "$system_stage" "$i" >&2
                else
                    printf 'Occupied kernel destination; prior bundle preserved at %s/old-%s.kext\n' "$system_stage" "$i" >&2
                fi
            fi
            if [ "${system_old_receipt[$i]:-0}" -eq 1 ]; then
                if [ ! -e "${system_receipts[$i]}" ] && [ ! -L "${system_receipts[$i]}" ]; then
                    /usr/bin/sudo -n /bin/mv "$system_stage/old-receipt-$i" "${system_receipts[$i]}" ||
                        printf 'Prior kernel receipt preserved at %s/old-receipt-%s\n' "$system_stage" "$i" >&2
                else
                    printf 'Occupied kernel receipt; prior receipt preserved at %s/old-receipt-%s\n' "$system_stage" "$i" >&2
                fi
            fi
        done
        if [ "$user_published" -eq 1 ]; then
            remove_registered "$prefix" "$temporary/new-files" ||
                printf 'Rollback kept modified files at %s; prior files remain at %s\n' "$prefix" "$previous" >&2
        fi
        if [ -n "$previous" ] && [ -d "$previous/userspace" ]; then
            if [ ! -e "$prefix" ] && [ ! -L "$prefix" ]; then /bin/mv "$previous/userspace" "$prefix";
            else printf 'Prior installation preserved at %s/userspace\n' "$previous" >&2; fi
        fi
    fi
    if [ -n "$system_stage" ]; then
        if [ "$committed" -eq 1 ]; then
            /usr/bin/sudo -n /bin/rm -rf "$system_stage"
        elif old_paths=$(/usr/bin/sudo -n /usr/bin/find "$system_stage" -name 'old-*' -print -quit); then
            if [ -z "$old_paths" ]; then /usr/bin/sudo -n /bin/rm -rf "$system_stage";
            else printf 'Kernel rollback files preserved at %s\n' "$system_stage" >&2; fi
        else
            # Losing sudo access or failing to inspect staging is uncertainty,
            # never evidence that the prior installation was restored.
            printf 'Kernel staging could not be inspected; preserved at %s\n' "$system_stage" >&2
        fi
    fi
    if [ "$system_locked" -eq 1 ]; then /usr/bin/sudo -n /bin/rmdir /Library/Extensions/.Mellow-installer.lock; fi
    [ -z "$stage" ] || /bin/rm -rf "$stage"
    [ -z "$previous" ] || /bin/rmdir "$previous" 2>/dev/null
    [ -z "$lock" ] || /bin/rmdir "$lock" 2>/dev/null
    [ -z "$temporary" ] || /bin/rm -rf "$temporary"
    exit "$result"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --help|-h) usage; exit 0;;
        --archive) argument "$@"; choose_source archive "$2"; shift 2;;
        --version|--tag) argument "$@"; choose_source version "$2"; shift 2;;
        --url) argument "$@"; choose_source url "$2"; shift 2;;
        --sha256) argument "$@"; expected_sha=$2; shift 2;;
        --checksum-file) argument "$@"; checksum_file=$2; shift 2;;
        --prefix) argument "$@"; prefix=$2; shift 2;;
        --install-kext) install_kext=1; shift;;
        --install-dependencies) install_dependencies=1; shift;;
        --prepare-kext) prepare_kext=1; shift;;
        *) die "Unknown option: $1";;
    esac
done
[ -n "$source_kind" ] || { usage; die 'An archive, release version or URL is required.'; }
[ "$(/usr/bin/uname -s)" = Darwin ] || die 'This installer runs on macOS only.'
[ "$(/usr/bin/id -u)" != 0 ] || die 'Run as your normal user, not sudo bash.'
arm=$(/usr/sbin/sysctl -n hw.optional.arm64 2>/dev/null || printf 0)
translated=$(/usr/sbin/sysctl -in sysctl.proc_translated 2>/dev/null || printf 0)
[ "$(/usr/bin/uname -m)" = x86_64 ] && [ "$arm" != 1 ] && [ "$translated" != 1 ] ||
    die 'This release requires Intel x86_64 macOS; Apple silicon/Rosetta is refused.'
host_version=$(/usr/bin/sw_vers -productVersion)
version_at_least "$host_version" 15.0 || die 'This package requires macOS 15.0 or later.'
prefix=${prefix%/}
safe_absolute "$prefix" || die 'Prefix must be an absolute non-root path without symlink ancestors or dot components.'
case "$prefix" in /System|/System/*|/Library/Extensions|/Library/Extensions/*) die 'That prefix is reserved for the operating system.';; esac
[ "$install_dependencies" -eq 0 ] || [ "$install_kext" -eq 1 ] || die '--install-dependencies requires --install-kext.'
[ "$prepare_kext" -eq 0 ] || [ "$install_kext" -eq 1 ] || die '--prepare-kext requires --install-kext.'
[ -z "$expected_sha" ] || valid_hash "$expected_sha" || die 'Invalid ZIP SHA-256.'
[ -z "$expected_sha" ] || [ -z "$checksum_file" ] || die 'Choose --sha256 or --checksum-file.'
temporary=$(/usr/bin/mktemp -d /private/tmp/mellow-install.XXXXXXXX)
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 129' HUP
trap 'exit 143' TERM
archive="$temporary/$ASSET"
if [ "$source_kind" = archive ]; then
    [ -f "$source_value" ] || die 'Local archive does not exist or is not a regular file.'
    /bin/cp "$source_value" "$archive"
else
    if [ "$source_kind" = version ]; then
        [[ "$source_value" =~ ^[A-Za-z0-9_.+-]+$ ]] || die 'Invalid release tag.'
        url="https://github.com/$REPOSITORY/releases/download/$source_value/$ASSET"
    else url=$source_value; fi
    [[ "$url" =~ ^https://[^/?#@]+/[^?#]+$ ]] && [[ "$url" != *$'\n'* ]] && [[ "$url" != *$'\r'* ]] ||
        die 'Use an HTTPS URL without credentials, query strings or fragments.'
    /usr/bin/curl --fail --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
        --connect-timeout 20 --max-time 900 --max-filesize 536870912 --retry 2 --output "$archive" "$url"
    if [ -z "$expected_sha" ] && [ -z "$checksum_file" ]; then
        checksum_file="$temporary/external.sha256"
        /usr/bin/curl --fail --location --proto '=https' --proto-redir '=https' --tlsv1.2 \
            --connect-timeout 20 --max-time 60 --max-filesize 65536 --output "$checksum_file" "$url.sha256"
    fi
fi
[ "$(/usr/bin/stat -f %z "$archive")" -le 536870912 ] || die 'Archive exceeds the 512 MiB limit.'
if [ -n "$checksum_file" ]; then
    [ -f "$checksum_file" ] && [ "$(/usr/bin/stat -f %z "$checksum_file")" -le 65536 ] || die 'Invalid external checksum file.'
    matches=0
    while IFS= read -r line || [ -n "$line" ]; do
        if checksum_line "$line" && [ "$line_name" = "$ASSET" ]; then expected_sha=$line_sha; matches=$((matches+1)); fi
    done < "$checksum_file"
    [ "$matches" -eq 1 ] || die 'External checksum must contain exactly one entry for Mellow-macos-x86_64.zip.'
fi
if [ -n "$expected_sha" ]; then
    expected_sha=$(printf '%s' "$expected_sha" | /usr/bin/tr 'A-F' 'a-f')
    [ "$(hash_file "$archive")" = "$expected_sha" ] || die 'ZIP checksum mismatch; nothing installed.'
else
    printf 'Local archive has no external digest; internal checksums establish consistency, not publisher authenticity.\n' >&2
fi

/usr/bin/zipinfo -1 "$archive" > "$temporary/entries"
/usr/bin/zipinfo -l "$archive" > "$temporary/attributes"
entries=0
: > "$temporary/names"
while IFS= read -r name || [ -n "$name" ]; do
    name=${name%/}
    valid_relative "$name" || die 'ZIP contains an unsafe path.'
    case "$name" in manifest.json|SHA256SUMS|Mellow.kext|Mellow.kext/*|frameworks|frameworks/*|bin|bin/*|dependencies|dependencies/*|licenses|licenses/*) ;; *) die "Unexpected archive root: $name";; esac
    printf '%s\n' "$name" >> "$temporary/names"
    entries=$((entries+1)); [ "$entries" -le 65536 ] || die 'Too many ZIP entries.'
done < "$temporary/entries"
[ "$entries" -gt 0 ] || die 'Empty archive.'
/usr/bin/tr 'A-Z' 'a-z' < "$temporary/names" | /usr/bin/sort | /usr/bin/uniq -d > "$temporary/duplicates"
[ ! -s "$temporary/duplicates" ] || die 'ZIP contains duplicate or case-colliding paths.'
metadata_entries=0 unpacked=0
while IFS= read -r line || [ -n "$line" ]; do
    if [[ "$line" =~ ^([^[:space:]]{10})[[:space:]] ]]; then
        read -r permissions format origin bytes remainder <<< "$line"
        case "$permissions" in -?????????|d?????????) ;; *) die 'ZIP symlinks and special files are refused.';; esac
        [[ "$bytes" =~ ^[0-9]{1,10}$ ]] || die 'Invalid ZIP size metadata.'
        unpacked=$((unpacked+10#$bytes)); [ "$unpacked" -le 2147483648 ] || die 'Expanded archive exceeds 2 GiB.'
        metadata_entries=$((metadata_entries+1))
    fi
done < "$temporary/attributes"
[ "$metadata_entries" -eq "$entries" ] || die 'Ambiguous ZIP metadata.'
payload="$temporary/payload"
/bin/mkdir "$payload"
/usr/bin/unzip -qq "$archive" -d "$payload"
/usr/bin/find "$payload" ! -type d ! -type f -print > "$temporary/special"
[ ! -s "$temporary/special" ] || die 'Extracted payload contains special files or symlinks.'
[ -f "$payload/SHA256SUMS" ] && [ -f "$payload/manifest.json" ] || die 'Release metadata missing.'
[ "$(/usr/bin/stat -f %z "$payload/SHA256SUMS")" -le 16777216 ] || die 'Checksum table too large.'
[ "$(/usr/bin/stat -f %z "$payload/manifest.json")" -le 16777216 ] || die 'Manifest too large.'
: > "$temporary/checksummed"
while IFS= read -r line || [ -n "$line" ]; do
    checksum_line "$line" || die 'Malformed internal checksum entry.'
    [ "$line_name" != SHA256SUMS ] && [ -f "$payload/$line_name" ] || die 'Checksum names an absent/non-file payload.'
    printf '%s\n' "$line_name" >> "$temporary/checksummed"
done < "$payload/SHA256SUMS"
/usr/bin/sort "$temporary/checksummed" > "$temporary/checksummed.sorted"
/usr/bin/uniq -d "$temporary/checksummed.sorted" > "$temporary/duplicates"
[ ! -s "$temporary/duplicates" ] || die 'Duplicate checksum entry.'
: > "$temporary/files"
while IFS= read -r -d '' path; do
    name=${path#"$payload/"}
    valid_relative "$name" || die 'Extracted filename is unsafe.'
    [ "$name" = SHA256SUMS ] || printf '%s\n' "$name" >> "$temporary/files"
done < <(/usr/bin/find "$payload" -type f -print0)
/usr/bin/sort "$temporary/files" > "$temporary/files.sorted"
/usr/bin/cmp -s "$temporary/files.sorted" "$temporary/checksummed.sorted" || die 'Checksums do not cover every payload file.'
(cd "$payload" && /usr/bin/shasum -a 256 -c SHA256SUMS > "$temporary/checksum-verification") || die 'Payload checksum mismatch.'

manifest="$payload/manifest.json"
/usr/bin/plutil -lint "$manifest" >/dev/null || die 'Invalid manifest.'
[ "$(field "$manifest" schema_version integer)" = 1 ] &&
[ "$(field "$manifest" package_name string)" = Mellow ] &&
[ "$(field "$manifest" asset_name string)" = "$ASSET" ] &&
[ "$(field "$manifest" repository string)" = "$REPOSITORY" ] &&
[ "$(field "$manifest" architecture string)" = x86_64 ] || die 'Release manifest identity mismatch.'
for key in binary_compiled experimental; do [ "$(field "$manifest" "$key" bool)" = true ] || die "Invalid $key status."; done
for key in signed developer_id_signed notarized hardware_support_verified system_metal_registered windowserver_acceleration_verified; do
    [ "$(field "$manifest" "$key" bool)" = false ] || die "Unexpected $key; this installer handles development packages only."
done
minimum=$(field "$manifest" minimum_macos string)
version_at_least "$minimum" 15.0 && version_at_least "$host_version" "$minimum" || die "This archive requires macOS $minimum or later."
commit=$(field "$manifest" source_commit string)
[[ "$commit" =~ ^[a-f0-9]{40}$ ]] || die 'Invalid source commit.'
if [ "$source_kind" = version ]; then [ "$(field "$manifest" release_tag string)" = "$source_value" ] || die 'Release tag mismatch.'; fi
[ -f "$payload/frameworks/MellowAppleUserspace.framework/MellowAppleUserspace" ] &&
[ -f "$payload/bin/metal-inventory" ] && [ -f "$payload/bin/libMellowAppleUserspace.dylib" ] || die 'Native user-space artifacts missing.'
printf 'Experimental Mellow source %s: native GPU/Metal/WindowServer support remains unverified.\n' "$commit"
printf 'No Developer ID signing or notarization; macOS may refuse kernel installation/loading.\n'

parent=${prefix%/*}; [ -n "$parent" ] || parent=/
/bin/mkdir -p "$parent"
safe_absolute "$prefix" && [ -w "$parent" ] || die 'Prefix parent must be writable without sudo and have no symlink ancestors.'
lock="$prefix.mellow-lock"
/bin/mkdir "$lock" || { lock=; die 'Another install/uninstall holds this prefix lock. Inspect a leftover lock manually.'; }
: > "$temporary/prior-files"
if [ -e "$prefix" ]; then
    [ -d "$prefix" ] || die 'Prefix is not a directory.'
    if [ -f "$prefix/.mellow-files" ]; then
        IFS= read -r magic < "$prefix/.mellow-files"
        [ "$magic" = 'Mellow userspace receipt v1' ] || die 'Unknown installation receipt.'
        /usr/bin/tail -n +2 "$prefix/.mellow-files" > "$temporary/prior-files"
        : > "$temporary/prior-names"
        while IFS= read -r line || [ -n "$line" ]; do
            checksum_line "$line" && user_relative "$line_name" || die 'Invalid prior receipt.'
            safe_absolute "$prefix/$line_name" && [ -f "$prefix/$line_name" ] &&
                [ "$(hash_file "$prefix/$line_name")" = "$line_sha" ] || die 'Prior files were changed; uninstall preserves them before a fresh install.'
            printf '%s\n' "$line_name" >> "$temporary/prior-names"
        done < "$temporary/prior-files"
        printf '.mellow-files\n' >> "$temporary/prior-names"
        : > "$temporary/prior-actual"
        while IFS= read -r -d '' path; do printf '%s\n' "${path#"$prefix/"}" >> "$temporary/prior-actual"; done < <(/usr/bin/find "$prefix" ! -type d -print0)
        /usr/bin/sort "$temporary/prior-names" > "$temporary/prior-names.sorted"
        /usr/bin/sort "$temporary/prior-actual" > "$temporary/prior-actual.sorted"
        /usr/bin/cmp -s "$temporary/prior-names.sorted" "$temporary/prior-actual.sorted" || die 'Prefix contains unregistered files; preserve them outside it before upgrading.'
    else
        [ -z "$(/usr/bin/find "$prefix" -mindepth 1 -print -quit)" ] || die 'Nonempty unmanaged prefix refused.'
    fi
fi
stage=$(/usr/bin/mktemp -d "$parent/.mellow-install.XXXXXXXX")
/usr/bin/ditto "$payload/frameworks" "$stage/Frameworks"
/usr/bin/ditto "$payload/bin" "$stage/bin"
/usr/bin/ditto "$payload/licenses" "$stage/licenses"
/bin/cp "$manifest" "$payload/SHA256SUMS" "$stage/"
: > "$temporary/new-files"
while IFS= read -r line || [ -n "$line" ]; do
    checksum_line "$line" || die 'Invalid verified checksum entry.'
    case "$line_name" in
        frameworks/*) line_name="Frameworks/${line_name#frameworks/}";;
        bin/*|licenses/*|manifest.json) ;;
        *) continue;;
    esac
    [ "$(hash_file "$stage/$line_name")" = "$line_sha" ] || die 'Staged user-space copy differs from verified payload.'
    printf '%s  %s\n' "$line_sha" "$line_name" >> "$temporary/new-files"
done < "$payload/SHA256SUMS"
printf '%s  SHA256SUMS\n' "$(hash_file "$stage/SHA256SUMS")" >> "$temporary/new-files"
{ printf 'Mellow userspace receipt v1\n'; /bin/cat "$temporary/new-files"; } > "$stage/.mellow-files"

stage_bundle() {
    local bundle=$1 identifier=$2 checksum_prefix=$3 index=${#system_targets[@]} line
    local target="/Library/Extensions/$bundle.kext" receipt="/Library/Extensions/.Mellow-installer-$bundle.receipt"
    [ "$(field "$payload/$checksum_prefix/Contents/Info.plist" CFBundleIdentifier string)" = "$identifier" ] || die "Unexpected $bundle identifier."
    if [ -e "$target" ] || [ -e "$receipt" ]; then
        [ -d "$target" ] && [ ! -L "$target" ] && [ -f "$receipt" ] && [ ! -L "$receipt" ] || die "Existing $bundle is not managed by this installer."
        { IFS= read -r magic; IFS= read -r recorded_id; } < "$receipt"
        [ "$magic" = 'Mellow managed bundle v1' ] && [ "$recorded_id" = "$identifier" ] || die "Existing $bundle has an unknown receipt."
        safe_absolute "$target/Contents/Info.plist" &&
            [ "$(field "$target/Contents/Info.plist" CFBundleIdentifier string)" = "$identifier" ] || die "Existing $bundle has a different bundle identity."
        local prior_registry="$temporary/system-prior-$index"
        local prior_names="$temporary/system-prior-names-$index"
        local prior_actual="$temporary/system-prior-actual-$index" path
        /usr/bin/tail -n +3 "$receipt" > "$prior_registry"
        : > "$prior_names"
        while IFS= read -r line || [ -n "$line" ]; do
            checksum_line "$line" || die 'Invalid system receipt.'
            case "$line_name" in Contents/*) ;; *) die 'Invalid system receipt path.';; esac
            safe_absolute "$target/$line_name" && [ -f "$target/$line_name" ] &&
                [ "$(hash_file "$target/$line_name")" = "$line_sha" ] || die "Existing $bundle was modified; refusing replacement."
            printf '%s\n' "$line_name" >> "$prior_names"
        done < "$prior_registry"
        [ -s "$prior_names" ] || die 'Empty system receipt.'
        # A managed bundle can still contain files added by its owner. Refuse
        # replacement rather than move those files into disposable staging.
        /usr/bin/find "$target" ! -type d -print0 > "$temporary/system-existing-$index"
        : > "$prior_actual"
        while IFS= read -r -d '' path; do
            local relative=${path#"$target/"}
            valid_relative "$relative" || die 'Existing bundle has an unsafe path.'
            printf '%s\n' "$relative" >> "$prior_actual"
        done < "$temporary/system-existing-$index"
        /usr/bin/sort "$prior_names" > "$prior_names.sorted"
        /usr/bin/sort "$prior_actual" > "$prior_actual.sorted"
        /usr/bin/cmp -s "$prior_names.sorted" "$prior_actual.sorted" || die "Existing $bundle contains unregistered files; refusing replacement."
    fi
    system_targets[$index]=$target system_receipts[$index]=$receipt
    system_new[$index]=0 system_old[$index]=0 system_new_receipt[$index]=0 system_old_receipt[$index]=0
    /usr/bin/sudo -n /usr/bin/ditto "$payload/$checksum_prefix" "$system_stage/new-$index.kext"
    local local_receipt="$temporary/bundle-receipt-$index"
    printf 'Mellow managed bundle v1\n%s\n' "$identifier" > "$local_receipt"
    while IFS= read -r line || [ -n "$line" ]; do
        checksum_line "$line" || die 'Invalid verified checksum.'
        case "$line_name" in "$checksum_prefix"/*)
            local relative=${line_name#"$checksum_prefix/"}
            local actual
            actual=$(/usr/bin/sudo -n /usr/bin/shasum -a 256 "$system_stage/new-$index.kext/$relative" | /usr/bin/awk '{print $1}')
            [ "$actual" = "$line_sha" ] || die 'Privileged staged copy checksum mismatch.'
            printf '%s  %s\n' "$line_sha" "$relative" >> "$local_receipt";;
        esac
    done < "$payload/SHA256SUMS"
    /usr/bin/sudo -n /bin/cp "$local_receipt" "$system_stage/new-receipt-$index"
    /usr/bin/sudo -n /usr/sbin/chown -R root:wheel "$system_stage/new-$index.kext" "$system_stage/new-receipt-$index"
    /usr/bin/sudo -n /bin/chmod -R u+rwX,go+rX,go-w "$system_stage/new-$index.kext"
    /usr/bin/sudo -n /bin/chmod 644 "$system_stage/new-receipt-$index"
}
if [ "$install_kext" -eq 1 ]; then
    safe_absolute /Library/Extensions && [ -d /Library/Extensions ] &&
        [ "$(/usr/bin/stat -f %u /Library/Extensions)" = 0 ] || die 'System extension directory is unsafe or unavailable.'
    if [ -e /Library/Extensions/Lilu.kext ]; then
        safe_absolute /Library/Extensions/Lilu.kext || die 'Existing Lilu path is a symlink.'
        [ "$(field /Library/Extensions/Lilu.kext/Contents/Info.plist CFBundleIdentifier string)" = as.vit9696.Lilu ] || die 'Existing Lilu identifier mismatch.'
        lilu_version=$(field /Library/Extensions/Lilu.kext/Contents/Info.plist CFBundleVersion string)
        version_at_least "$lilu_version" 1.6.4 || die 'Existing Lilu is older than 1.6.4; update it separately. It was not changed.'
    elif [ "$install_dependencies" -eq 0 ]; then
        die 'Mellow.kext requires Lilu >=1.6.4 in /Library/Extensions. Add --install-dependencies to explicitly install bundled Lilu if absent.'
    fi
    /usr/bin/sudo -v
    /usr/bin/sudo -n /bin/mkdir /Library/Extensions/.Mellow-installer.lock || die 'Kernel installation lock is held; no system files replaced.'
    system_locked=1
    system_stage=$(/usr/bin/sudo -n /usr/bin/mktemp -d /Library/Extensions/.Mellow-install.XXXXXXXX)
    if [ ! -e /Library/Extensions/Lilu.kext ]; then
        lilu_version=$(field "$payload/dependencies/Lilu.kext/Contents/Info.plist" CFBundleVersion string)
        version_at_least "$lilu_version" 1.6.4 || die 'Bundled Lilu is too old.'
        stage_bundle Lilu as.vit9696.Lilu dependencies/Lilu.kext
    fi
    stage_bundle Mellow com.NiSeullent.Mellow Mellow.kext
fi

if [ -d "$prefix" ]; then
    previous=$(/usr/bin/mktemp -d "$parent/.mellow-backup.XXXXXXXX")
    /bin/mv "$prefix" "$previous/userspace"
fi
/bin/mv "$stage" "$prefix"
stage= user_published=1
for ((i=0;i<${#system_targets[@]};i++)); do
    if [ -d "${system_targets[$i]}" ]; then
        /usr/bin/sudo -n /bin/mv "${system_targets[$i]}" "$system_stage/old-$i.kext"
        system_old[$i]=1
        /usr/bin/sudo -n /bin/mv "${system_receipts[$i]}" "$system_stage/old-receipt-$i"
        system_old_receipt[$i]=1
    fi
    /usr/bin/sudo -n /bin/mv "$system_stage/new-$i.kext" "${system_targets[$i]}"
    system_new[$i]=1
    /usr/bin/sudo -n /bin/mv "$system_stage/new-receipt-$i" "${system_receipts[$i]}"
    system_new_receipt[$i]=1
done
committed=1
if [ -n "$previous" ]; then
    if [ -s "$temporary/prior-files" ]; then
        remove_registered "$previous/userspace" "$temporary/prior-files" || printf 'Prior backup retained at %s/userspace\n' "$previous" >&2
    else /bin/rmdir "$previous/userspace" 2>/dev/null || true; fi
fi
printf 'Installed user-space files at %s\n' "$prefix"
printf 'Read-only inventory: "%s/bin/metal-inventory"\n' "$prefix"
printf 'No GPU test or window was run. No shell PATH or system Metal registration changed.\n'
if [ "$install_kext" -eq 1 ]; then
    printf 'Kernel bundles copied to /Library/Extensions; loading, approval and reboot remain separate.\n'
fi
if [ "$prepare_kext" -eq 1 ]; then
    printf 'Requesting standard macOS kernel-cache preparation; installed files remain if this fails.\n'
    if ! /usr/bin/sudo -n /usr/bin/kmutil install --volume-root / --check-rebuild; then
        printf 'Kernel preparation failed. File installation completed; no security setting was changed and no automatic reboot was attempted.\n' >&2
        exit 3
    fi
fi

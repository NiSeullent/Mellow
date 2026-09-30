# Official Mellow static website

This directory is the static, Korean-first website for NiSeullent/Mellow. There
is no package installation, application server, build dependency, tracking,
credential input or automatic installation. The chip illustration is original
CSS and the small brand mark is original SVG.

The HTML, stylesheet and script use relative asset/data URLs. GitHub's API
reports the repository name as `Mellow`; project Pages ordinarily uses
`/Mellow/`. Pages was not enabled at the time of the read-only API check.
The actual published URL must come from repository Pages metadata after the
repository owner enables it. No deployment or Pages setting was changed by
the website author.

## Data ownership and scope

`data/compatibility.json` is owned by the compatibility-data agent/generator,
not the page author. The site accepts the agreed schemaVersion 1,
catalogScope `source-families`, camelCase devices and three false hardware/system
claims. A family profile, model example or target OS never becomes verified
runtime compatibility. Source paths are never appended to a revision to invent
a blob URL; provided sourceLinks are used as independent provenance.

The optional `data/models.json` lists official source declarations. A null
familyId displays as unclassified. PCI declarations remain separate from empty
measured IDs; OEM/subsystem-qualified declarations are explicitly labeled.
The current model catalog is not exhaustive. Missing or invalid model data
disables that view without generating invented models.

Search and filters cover the full dataset. At most 24 matched cards are initially
rendered, with independent total/matched/shown counts and a show-more button.
Changing filters or catalog mode resets that display bound. Hexadecimal PCI
queries accept an optional 0x prefix. The current data has 44 source families
and 838 official source model declarations; these counts are data-derived,
never hardcoded as a compatibility or verification claim in the page.

The browser renders all metadata via textContent/DOM nodes, accepts HTTPS source
links and restricts download URLs to the official repository release paths.
Malformed data, API timeouts, request limits and missing assets have explicit
unavailable states. No cached fixture or invented download is presented.

## Real downloads and CLI

The page reads the unauthenticated official GitHub releases API, with at most
30 recent releases and a link to the complete release history. Only actual
published API assets have download links. The selector chooses the newest
complete compiled package described below when available, otherwise a
recognizable native development asset;
users can choose any other returned release.

Root-planned release assets are `mellow-install.sh`,
`mellow-installer-macos15-x86_64` / `mellow-installer-macos26-x86_64`, and
`mellow-development-macos15-x86_64.tar` / `mellow-development-macos26-x86_64.tar`.
Their links appear only after those actual assets exist. The downloadable
MellowAppleUserspace.framework is an explicitly selected existing-host app
adapter, not an Apple system Metal device implementation. The displayed shell
command downloads an API-returned script URL and prints help. The page never
runs it, automatically activates a kext, changes system settings or claims GPU
execution.

Current installer and newer scope-document source links target
`codex/mellow-gpu-port-1edd` because source PR 3 has not yet been merged.
The release descriptions and source checkpoint in each payload are controlling.

### Additive compiled ZIP contract

The parallel family-routing build defines a stock-tools installer and one
macOS 15/26 Intel x86_64 package, published only after its required binary
build succeeds. Its five required assets are `Mellow-macos-x86_64.zip`,
`Mellow-macos-x86_64.zip.sha256`, `manifest.json`, `install-mellow.sh` and
`uninstall-mellow.sh`.
The page enables this package's CTA only when all five names occur exactly
once, have nonzero uploaded API assets and belong to the selected official
release tag. A partial upload cannot enable package or installer controls.
Among complete packages, the newest valid `published_at` is selected first;
the older `mellow-install.sh`/per-OS tar contract remains available unchanged.

The new installer UI has two separate command/copy boxes: save and review
`install-mellow.sh`, then run `bash ./install-mellow.sh --version TAG` after
review. API-returned URLs and tags are shell-quoted, never executed by the page.
The default prefix is `~/Library/Mellow`. External ZIP and internal payload
checksums are verified by the installer. System kext/dependency installation,
kmutil preparation and GPU acceptance are separate explicit steps; downloading
or installing a compiled package does not verify GPU/Metal/WindowServer support.
Its installation-document link uses the exact selected release tag. Package
availability is discovered live, not inferred from build source or CI progress.

### Cross-session publication ownership

This additive integration starts from the actual public
`codex/mellow-pages@5435ff5dddddca9c0e57ece0af123403f1295e68` parent in the isolated
`codex/mellow-download-integration-20260930` branch. Only `assets/app.js` and
this README are changed; the 44-family/838-declaration data, provenance,
homepage markup, styling and MIT notice are preserved. The family-routing
session's separate 26-entry site is a reviewed alternative, not the currently
published catalog. Publication owners must review the current public parent
and coordinate one deployment before updating the shared Pages branch/config;
independent automatic deployments must not overwrite each other's site.
The family-routing source records this handoff in
`docs/PUBLICATION-COORDINATION.md`; that record does not imply acknowledgment
by an independent peer session.

Review complete/missing/zero-size/non-uploaded/duplicate/cross-tag package
assets, newest-package selection, API failure and release switching. Confirm
the native run box disappears when switching to an older installer, both old
and new command contracts remain correct, and all catalog validation claims
stay false. Direct syntax/data/browser checks and publication are performed by
the parent coordinator; no repository scripts were run for this integration.

## Publication

`.github/workflows/pages.yml` is workflow_dispatch-only. Merely adding or
merging it does not deploy anything. The repository owner enables Pages,
selects an appropriate protected publication source/environment and dispatches
the workflow, or publishes these static files at the root of a dedicated
`codex/mellow-pages` branch. Relative URLs support that directory layout.
The workflow never enables Pages automatically.

No repository scripts were run by the website author. Direct installed Node
syntax checking and direct Python HTML/data parsing can be used locally.
Actual responsive/search/download browser verification is delegated to root
using the already installed browser, without starting a service.

## License

The original website HTML/CSS/JS/SVG is MIT. Keep the accompanying LICENSE.MIT
when redistributing this site. The repository and downloadable native binaries
retain their separate root and per-file licenses and notices. Website MIT
licensing does not relicense Mellow runtime code, third-party code or firmware.

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
listed release with a recognizable native development asset when available;
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

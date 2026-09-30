# Shared Mellow publication

The official homepage is https://niseullent.github.io/Mellow/. The parallel
`codex/mellow-pages` branch owns its root publication and currently includes
44 family records and 838 model declarations. Model declarations are source
catalog entries, not evidence of working macOS acceleration.

`codex/mellow-family-routing-20260930` owns the direct-DMA source checkpoint,
the compiled-package pipeline in `packaging/`, and the stock-macOS shell
installer. Its `website/` is a reviewed alternative; its Pages workflow is
manual-only after overlapping deployments were discovered on 2026-09-30.
Routine source commits must not replace the shared homepage or change its
Pages configuration.

The isolated `codex/mellow-download-integration-20260930` branch starts from
the published Pages commit `5435ff5dddddca9c0e57ece0af123403f1295e68` and adds
the shell-package download contract to that existing homepage. Publication
updates preserve its catalog and prior installer contract. A normal fast
forward is required; an intervening publication must be fetched and combined.
Peer development checkouts are not edited here, and no peer acknowledgment
is claimed.

The package contract is a single release containing uploaded, nonempty
`Mellow-macos-x86_64.zip`, `Mellow-macos-x86_64.zip.sha256`, `manifest.json`,
`install-mellow.sh`, and `uninstall-mellow.sh`. The ZIP manifest identifies
the exact compiled source commit and release tag. Install with the saved
script and `--version TAG`; the default destination is `~/Library/Mellow`.
These assets differ from the parallel tar/Swift installer contract and must
not be interpreted using its options or manifest schema.

No package, catalog, host test, or website deployment establishes native GPU
execution, system Metal registration, WindowServer acceleration, or scanout.
Those implementation goals remain open.

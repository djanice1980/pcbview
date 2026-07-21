# Test corpus

Every directory here is a board package that `pcbview_tests` imports and
holds to survival invariants: no exception, a real outline, copper present,
a successful assemble. Ground truth is not required -- the corpus exists to
feed the parsers the wild. The harness dispatches by content: a folder with
`matrix/matrix` is an ODB++ job, a folder holding an IPC-2581 `.xml` is
that, anything else is a Gerber package.

Current packages:

- `synthetic-9x5/` -- the hand-authored fixture board as gerbers.
- `pic-programmer-gerber/`, `pic-programmer-2581/` -- KiCad's bundled
  `pic_programmer` demo, exported with kicad-cli 10.0.4 (gerbers+drill,
  IPC-2581).
- `stickhub-odb/` -- KiCad's bundled `stickhub` demo, exported with
  kicad-cli as an ODB++ job.

The KiCad demo exports are derived from the demo projects that ship with
KiCad (gitlab.com/kicad/code/kicad, GPL-3-compatible licensing) --
regenerate any of them with `kicad-cli pcb export <format>` against the
installed demos.

To grow coverage: drop a package folder in and commit it. Rules:

- Only content that can live in a public GPL-3 repository: synthetic
  packages, gerbers plotted from GPL/CC-licensed open-hardware designs
  (KiCad's demo projects are a good source -- plot them with kicad-cli),
  or packages whose owner has agreed.
- Keep packages small where possible; the point is FORMAT coverage
  (aperture macros, arcs, step-repeat, odd drill files), not board size.
- When a package exposed a real bug, say so in a NOTE.md beside it so the
  regression it guards is findable.

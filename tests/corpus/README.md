# Test corpus

Every directory here is a Gerber package that `pcbview_tests` imports and
holds to survival invariants: no exception, a real outline, copper present,
a successful assemble. Ground truth is not required -- the corpus exists to
feed the parser the wild.

To grow coverage: drop a package folder in and commit it. Rules:

- Only content that can live in a public GPL-3 repository: synthetic
  packages, gerbers plotted from GPL/CC-licensed open-hardware designs
  (KiCad's demo projects are a good source -- plot them with kicad-cli),
  or packages whose owner has agreed.
- Keep packages small where possible; the point is FORMAT coverage
  (aperture macros, arcs, step-repeat, odd drill files), not board size.
- When a package exposed a real bug, say so in a NOTE.md beside it so the
  regression it guards is findable.

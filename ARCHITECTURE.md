# pcbview

Standalone, portable 3D PCB viewer. Rasterized now, hardware ray traced later.

Takes a KiCad project **or** Gerber files and renders the board in 3D. No KiCad
install required.

> **Amended 2026-07-16 — "single executable" no longer holds.** The GUI is Qt,
> used under LGPL-3.0, which requires dynamic linking (see
> [NOTICE.md](NOTICE.md)). pcbview now ships as a **portable folder**: the
> executable plus Qt's DLLs, no registry, xcopy-deployable.
> Everything else stands — no KiCad install, no package manager, native parsers.
> This was decided knowingly, with the cost on the table.
>
> **Amended 2026-07-19 — an installer now exists *alongside* the portable
> folder, not instead of it.** `installer/pcbview.iss` (Inno Setup) wraps the
> deployed `build/Release` into `pcbview-<version>-setup.exe` (Start Menu +
> optional desktop shortcut + uninstaller; per-machine or per-user). The
> portable zip remains a first-class release artifact, and the app itself still
> touches no registry — settings stay in `~/.pcbview/settings.xml`.

## The governing principle

**Render what the fab will build, not what KiCad draws.**

Where the two disagree, fabrication output wins — the drill program, the Gerbers,
the physical stackup. KiCad's 3D viewer is a useful cross-check and a convenient
source of ground truth, but it is *not* the target. It has already been shown to
contradict KiCad's own fab output on a real board (see "Via type vs layer span"),
and matching it there would mean rendering a board that cannot be manufactured.

This settles ambiguities without re-litigating them. When a question arises, ask
what comes out of the plant.

Stated by the project's owner, 2026-07-16.

## Decisions (locked 2026-07-16)

| Axis | Choice | Why |
|---|---|---|
| Geometry ingestion | Native parsers | Portability; no `kicad-cli` dependency |
| Input formats | KiCad **and** Gerber from the start | Neither becomes a retrofit; they cross-validate |
| Renderer foundation | C++ / Vulkan | Only route that guarantees hardware RT in phase 4 |
| Dependencies | CMake `FetchContent` | No vcpkg install; CMake ships with VS 18 |
| GUI (2026-07-16) | **Qt Widgets, LGPL-3.0, dynamically linked** | Real menus/docks/native dialogs; costs the single-exe goal |
| Component models | WRL first, STEP deferred | STEP means OpenCASCADE, which undercuts portability |

### Licence compliance is a build constraint, not paperwork

Qt is used under **LGPL-3.0 via option 4(d)(1)** (shared library mechanism).
[NOTICE.md](NOTICE.md) records the full obligation-by-obligation mapping. The
rules that constrain code:

- **Never statically link Qt.** Doing so abandons 4(d)(1) and forces 4(d)(0),
  which obliges us to ship relinkable Corresponding Application Code.
- **Never modify Qt.** That triggers §2 and obliges conveying its source.
- **Never do anything that stops a user swapping the Qt DLLs** — no checksums
  over them, no signature pinning. §4's chapeau forbids terms that restrict
  modification of the Library or reverse engineering for debugging it.
- **Only LGPL Qt modules.** Some Qt modules are GPL-3.0-or-commercial only;
  linking one would place all of pcbview under the GPL. Qt Core / Gui / Widgets
  are LGPL-3.0 — verify anything else before adding it.
- **`--static` on Qt is a licence violation here, not an optimisation.** If a
  future contributor "fixes" the multi-file deploy by static linking, that is a
  legal regression, not a build improvement.

### Why not the easy paths

- **KiCad's own raytracer** is CPU-only and not realtime. `RENDER_3D_RAYTRACE_GL`
  exists in its source but does not use the GPU in practice. Nothing to reuse.
- **WebGPU / wgpu** has no shipping hardware ray tracing. Choosing it would give a
  fast phase 1 and strand phase 4.
- **Blender pipelines** (pcb2blender, gerber2blend) are minutes-per-frame Cycles
  renders, not viewers. Worth mining for technique, not code.

## Ray tracing (built) — ray-query shadows + AO

RT is implemented as **ray queries issued from the fragment shader**, not a
separate RT pipeline / SBT. It layers ray-traced contact shadows and ambient
occlusion on top of the raster shading, which is the biggest visual win for board
inspection (components read as *seated* instead of floating) at a fraction of the
machinery. The three readiness rules below paid off: it built over the existing
buffers and material table without reworking the asset layer.

- **Device.** `VK_KHR_ray_query` + `VK_KHR_acceleration_structure` (+ deferred host
  ops) enabled when present (`GpuInfo::rayQueryReady()`, `Device::rayQueryEnabled`).
  Both this box's GPUs qualify (RTX 5070 Ti and the Radeon 8060S / Strix Halo iGPU).
  Absent → raster fallback, no failure.
- **Acceleration structures** (`Renderer::buildAccelerationStructures`): one BLAS
  over the board's vertex/index buffers (already `SHADER_DEVICE_ADDRESS` +
  accel-build usage — rule 2), one TLAS with a single identity instance. Rebuilt
  each `uploadBoard`. Scratch is over-allocated by the alignment and the device
  address rounded up to `minAccelerationStructureScratchOffsetAlignment`.
- **Shader.** A *separate* `board_rt.frag` carries the `RayQueryKHR` capability
  (a non-RT device could not load it), selected at pipeline build when the device
  has ray_query. It is byte-identical to `board.frag` plus a shadow ray to the key
  light and a 6-tap hemisphere AO kernel. **Gotcha: ray query needs `#version 460`
  — 450 fails with "`rayQueryEXT` undeclared".** The TLAS binds at set 0, binding 1.
- **Gated to the collapsed board.** The BLAS is over the *un-exploded* geometry, so
  RT is traced only at rest (`explodeProgress_ < 0.01`); a peeling stack falls back
  to raster (its geometry is displaced in the vertex shader, not in the buffers).
  Toggled per-frame via `cameraPos.w` — no pipeline switch. `PCBVIEW_RT=1` forces
  it for a headless capture.
- **GPU selection.** `selectGpu(name-substring)` overrides the discrete+RT default;
  driven by `PCBVIEW_GPU`, the persisted `gpuName` setting, or the Render →
  Graphics device menu, which tears the device+renderer down and rebuilds on the
  chosen GPU (instance + surface kept, camera preserved). `PCBVIEW_GPU_REPORT=<file>`
  dumps the chosen device for verification.

## Path tracing + neural denoise (built)

An optional full render mode (`RenderMode::PathTraced`, `pathtrace.comp`): a
progressive Monte-Carlo path tracer for true global illumination, cleaned by
Intel Open Image Denoise.

- **Compute path tracer.** Cosine-weighted diffuse + a metallic mirror lobe,
  multi-bounce with Russian roulette, lit by a sky-dome + directional sun (rays
  that escape sample the environment -- that *is* the lighting). Accumulates one
  sample/frame into an RGBA32F image while the camera is still; the CPU resets on
  any camera/scene change. A fullscreen graphics pass tonemaps the average into
  `sceneColor_`, so the existing blit + UI present it.
  - **The tonemap must be identity below its knee** (`tonemap.frag`): the raster
    path never tonemaps — `board.frag` writes lit colour straight to the SRGB
    target — so any curve here is a colour DIFFERENCE between the modes. The
    original per-channel ACES lifted midtones ~40% and desaturated saturated
    colours (small channels rise proportionally more than the dominant one):
    the recurring "PT looks washed out / blown out" complaint was mostly ACES,
    not the lighting. Now: fixed exposure 0.85 (PT's sun+sky is ~1.1–1.25×
    albedo on a lit face vs raster's ≤1.0 rig), then a hue-preserving Reinhard
    shoulder on the MAX component from knee 0.8 — everything below the knee
    passes through linear, sun glints roll smoothly to 1 with no hue shift.
- **Hit shading fetches geometry itself** — rule 3 realised fully. `rayQuery`
  gives the primitive index + barycentrics; the shader reads the vertex/index
  SSBOs and a per-triangle material-index buffer (built in `uploadBoard`) to
  interpolate the normal and look up the material. **This is why the index buffer
  is GLOBAL** (indices rebased, `vertexOffset = 0`): one BLAS + the tracer address
  one flat vertex buffer. A per-part `vertexOffset` with local indices left the
  BLAS referencing the wrong vertices for every part but the first — it made the
  RT-shadow BLAS wrong too, undetected until the path tracer exposed it.
- **Firefly clamp is load-bearing.** A single specular bounce into the sun returns
  a huge value on a dark IC lid; capping per-sample radiance (`min(rad, 8)`) is the
  difference between grainy and clean — OIDN preserves un-clamped sparkle as
  "detail".
- **The RNG seed hash is load-bearing.** Pixel coordinates are XOR-chained
  through the hash finalizer, never mixed ADDITIVELY: `hashu(px.x + hashu(px.y
  + …))` builds correlation planes, which showed up as faint screen-anchored
  lines/squares of biased stochastic-alpha outcomes that SURVIVED CONVERGENCE
  (fixed columns got the same mask-lottery luck at every sample). The
  accumulation generation is also mixed into the seed so a restart gets a fresh
  stream instead of repainting the identical pattern.
- **OIDN denoise** (Apache-2.0, GPL-3-clean): a continuous ASYNCHRONOUS state
  machine (`Renderer::denoiseTick`, ticked every PT frame): a fenced GPU→host
  readback is submitted and *polled* — never waited — then a worker thread packs
  the guides and runs all of OIDN (including the ~0.4 s first-run filter
  commit), and the result lands in `ptDenoised_` when ready. The UI never
  stalls, and the image refines every ~150 ms while the camera is still. A
  kickoff needs ≥2 accumulated samples, which motion never reaches (accumulation
  resets per frame) — so a moving image is never denoised, hence no ghosting;
  `ptGeneration_` (bumped by `resetAccumulation()`, the single choke point for
  accumulation resets) additionally discards passes whose input went stale
  mid-flight. `abortDenoise()` drains the pass before anything it references is
  destroyed (shutdown, resize, denoise-off). The license-clean stand-in for DLSS
  Ray Reconstruction (which is NVIDIA-proprietary and GPL-incompatible).
  - **Device:** `OIDN_DEVICE_TYPE_DEFAULT` picks the fastest present — CUDA on the
    RTX 5070 Ti, HIP on the Radeon, else CPU (fallback on commit failure). GPU
    denoise is ~2.5× the CPU. **Must go through OIDN device buffers**
    (`oidnNewBuffer` + `oidnWriteBuffer`/`oidnReadBuffer`), not shared host
    pointers: a mapped Vulkan host buffer is not device-accessible for a CUDA/HIP
    device. The CPU + CUDA + HIP device DLLs are staged by CMake post-build.
- **Translucency = stochastic alpha via CANDIDATE traversal, one query.** The
  soldermask (and the substrate while it peels) commits only on its ENTERING face
  with probability `effAlpha`; opaque prims always commit; exit faces never do.
  Over samples a pixel converges to the raster blend `alpha*film +
  (1-alpha)*beneath`, with no epsilon-stepping — stepping past a film tunnelled
  through the 0.010 mm-thick layers (the mask bottom is COINCIDENT with the
  copper top it coats) and shaded masked traces from inside the copper.
  - **The BLAS is TWO geometry ranges** over one index buffer: uploadBoard emits
    opaque parts' triangles first, the translucent films (mask, substrate) last,
    and only the film range omits `VK_GEOMETRY_OPAQUE_BIT_KHR`. Opaque geometry
    traverses entirely on the hardware fast path; only film hits reach the
    candidate loop. (The first working version forced `gl_RayFlagsNoOpaqueEXT`
    on an all-opaque BLAS instead — correct but it pushed EVERY triangle through
    the shader, the single biggest PT cost.) Primitive indices are
    geometry-LOCAL: geometry 1's are re-based by `opaqueTriCount_`
    (`pc.counts.x`) before indexing idx/triMat — miss that and every film hit
    shades with the wrong material. Shadow rays add `TerminateOnFirstHitEXT`
    (occlusion is a boolean; closest-hit search was waste). Underlying trap,
    still true: with the OPAQUE geometry flag and `gl_RayFlagsNoneEXT`, hits
    auto-commit and `rayQueryProceedEXT` never yields candidates — a candidate
    filter on opaque-flagged geometry is silently dead code (bit us once).
  - GI bounce rays start `0.03` off the surface, which skips the thin films
    entirely — alpha only matters on camera/shadow rays, which is fine.
  - **The alpha dice is IDEMPOTENT per (ray, primitive), never a sequential
    draw.** The spec allows traversal to report the same triangle multiple
    times as a candidate (NVIDIA spatially splits large triangles across BVH
    cells and reports each crossing). A sequential `rnd()` rolled per report
    inflated the film's commit probability per BVH cell — the mask rendered in
    visibly different greens with seams along the world-axis split planes,
    NVIDIA-only, worst on large boards. The dice hashes (per-bounce seed ^
    primitive id), so every report of a primitive gets the same verdict; the
    film geometry additionally carries
    `VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR`. Any future
    candidate-loop logic must stay duplicate-report-safe.
- **Denoiser guides are ACCUMULATED, not sampled once.** With stochastic alpha, a
  single sample's first-hit albedo is a coin flip (film colour vs what's
  beneath); writing the guide from sample 0 alone hands OIDN a per-pixel noise
  mosaic that it edge-stops on and preserves — ghosted silk, hazy masked traces.
  Albedo + normal accumulate like the colour; `denoise()` divides by the sample
  count and re-normalises the normal.
- **Visibility is BAKED into the traced geometry.** A hidden part's vertices are
  set to NaN in `bakeExplode` — the spec defines a NaN-vertex triangle as
  *inactive*, so the BLAS drops it with no index surgery and the geometry
  ranges / primitive re-basing stay untouched. The bake also applies raster's
  collapsed-board rule (inner copper hidden at rest under an opaque substrate),
  so PT traces exactly raster's visible set. Visibility changes MUST go through
  `Renderer::setPartVisible` — a raw `PartInfo::visible` write only affects the
  raster draws. The rebuild trigger in drawFrame syncs the traced geometry for
  whichever consumer is about to trace (PT always; raster RT shadows at rest),
  which also cured a stale-exploded-TLAS bug for RT shadows.
- Env hooks: `PCBVIEW_PT=1`, `PCBVIEW_OIDN=1`, `PCBVIEW_PT_SPP=<n>`. Explode
  works: the BLAS is rebuilt from peel-baked vertices when the peel changes
  (`rebuildTracedGeometry`). Menu: Render → Path tracing / Neural denoise.

## CPU rendering (built) — lavapipe raster + Embree tracing

A full CPU device: **CPU Rendering (llvm)** in Render → Graphics device. Two
separate engines behind one menu entry, because neither can do the other's job:

- **Raster + present: Mesa lavapipe.** A prebuilt `vulkan_lvp.dll` (from
  mesa-dist-win, staged beside the exe with its ICD json) is registered
  *additively* at startup via `VK_ADD_DRIVER_FILES`, so it appears as one more
  Vulkan device next to the hardware GPUs and is never chosen automatically.
  The whole raster path — pipelines, reversed-Z, the material SSBO — runs on it
  unchanged.
- **All ray tracing: Intel Embree 4** (`src/render/cpu/cpu_tracer.cpp`).
  Lavapipe advertises no ray-query, and worse, **its BVH silently drops
  triangles on ~1M-triangle scenes** (measured; boards vanish piecemeal), so it
  cannot be trusted to trace even in software. SwiftShader has no RT extensions
  at all (verified via `VK_ICD_FILENAMES` isolation). Embree is the only
  reliable CPU traversal, so `CpuTracer` owns *both* traced modes when the CPU
  device is active: the RT preview and the path tracer. The renderer writes the
  traced image into per-frame-in-flight staging buffers (one per frame in
  flight — a single buffer raced the previous frame's copy) and copies it into
  the swapchain image.

Design rules that keep it correct:

- **The CPU path tracer is a line-for-line twin of `pathtrace.comp`.** Same
  RNG (`hashu`/LCG), same lighting rig, same sun-cone sampling, same laminate
  transmission, same material fetch. Any shader change MUST be mirrored — the
  user compares GPU and CPU renders side by side and has caught divergence.
- **The RT preview is a verbatim mirror of `board_rt.frag`**, not a cheap PT:
  camera-relative key/fill lighting (0.15/0.65/0.20), one traced key shadow,
  the fixed 6-ray AO kernel, no tonemap/exposure. Translucent films are
  composited analytically (raster alpha blend) rather than traced through —
  tracing them reused the stochastic film dice and let AO probes from film
  surfaces hit copper 35 µm below, which read as ambient blotches.
- **Stochastic alpha runs in Embree filter callbacks**
  (`rtcSetGeometryIntersectFilterFunction`), the CPU analogue of the GPU's
  candidate loop. The dice is idempotent per primitive
  (`hashu(diceSeed ^ prim)`) so duplicate filter invocations agree — same
  lesson as the NVIDIA BVH-split squares. **Do not replace the filters with a
  `tnear = t + eps` restart loop**: coincident surfaces (mask bottom on copper
  top) fall inside the epsilon per-triangle and produce blocky mottling.
- **Explode + visibility are baked** (`CpuTracer::bake`): displaced vertices
  for the peel, NaN vertices for hidden parts — the same inactive-triangle
  rule the GPU BLAS uses, driven off the same `PartSpan` ranks as
  `uploadBoard`.
- **Denoising is milestone-based** (power-of-two sample counts ≤ 32, then
  every 32), with the last denoised frame cached for display between
  milestones — OIDN on every progressive frame would starve the tracer.

**Switching device to/from CPU rebuilds the whole viewport widget.** On
Windows, swapping the presenting ICD (hardware GPU ↔ lavapipe) on the same
native HWND leaves the compositor stuck on the last pre-switch frame — the new
device renders (perf counters advance) but nothing presents. In-place
`QWindow::destroy()`/`create()` does NOT fix it and additionally breaks the
`createWindowContainer` embedding (blank client area). The only working fix is
what a manual app-restart-into-CPU-mode proved out: `MainWindow` tears down and
recreates the container + `VulkanWindow` (`rebuildViewport`, triggered by the
`viewportRebuildRequired` signal), restoring camera and explode state across
the swap and clearing the perf label (its 15-frame refresh throttle otherwise
shows the old device's numbers).

Fast movement (reduced-resolution render while the camera moves) defaults
**on** for the CPU device and **off** for GPUs, persisted independently
(`fastMovementCpu` / `fastMovementGpu`).

Settings live in **`~/.pcbview/settings.xml`** — a custom XML `QSettings`
format (`appSettings()`, `src/app/settings.cpp`) registered so the file is
human-readable and portable; nothing goes to the registry.

## The three RT-readiness rules

Phase 4 is only additive if phase 1 obeys these. All three are free in raster mode.

1. **Enable RT extensions at device creation, but never require them.** Query
   `VK_KHR_ray_tracing_pipeline` + `VK_KHR_acceleration_structure`, store a
   capability flag, fall back cleanly. The device is then never what blocks RT.
2. **Every mesh buffer carries `SHADER_DEVICE_ADDRESS` and
   `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY` usage from day one.** Phase 4
   then builds BLAS/TLAS over the exact buffers phase 1 already filled, instead
   of rebuilding the asset layer.
3. **Materials live in a bindless SSBO indexed by instance ID — never per-draw
   descriptor sets.** This is the rule that kills these projects when broken. A
   closest-hit shader cannot do per-draw binds; it receives an instance ID and
   must fetch material *and vertex attributes* itself via buffer device address.
   Write the rasterizer that way now, or phase 4 is a rewrite.

Corollary: one global vertex/index buffer with per-instance offsets. Not a buffer
per mesh.

**All three are implemented and load-bearing as of phase 1.** Rule 3 is realised
by passing the material index as `firstInstance` to `vkCmdDrawIndexed`, which the
vertex shader reads as `gl_InstanceIndex` and hands to the fragment shader. Phase
4 swaps that for `gl_InstanceCustomIndexEXT` in a closest-hit shader and the
material table is unchanged.

**The validation layer must have a debug messenger.** Enabling the layer without
`VK_EXT_debug_utils` is worse than not enabling it: it looks like coverage that
does not exist. Wiring the messenger caught a real bug on its first run — the
depth image was never transitioned out of `VK_IMAGE_LAYOUT_UNDEFINED`, which
NVIDIA happened to tolerate and another vendor would not have.

## Architecture

```
kicad/ ─► BoardModel ─► LayerArt ─► BoardMesh ─► renderer
              (semantic)   (polygons)   (triangles)
gerber/ ──────────────► LayerArt ─► BoardMesh ─► renderer
```

There are TWO meeting points, not one, and the reason is that Gerber has no
semantics to preserve:

- **`BoardModel`** (semantic): tracks, pads, vias, nets, zones, graphics, text.
  Only the KiCad importer produces it. A Gerber describes exposure, not intent —
  there is no net, pad, or via to recover — so Gerber cannot produce a
  BoardModel.
- **`LayerArt`** (`geom/layer_art.h`): filled polygons per physical layer, a
  profile, drills, and a stackup that places them in Z. This is where the two
  formats actually converge. `buildLayerArt()` resolves BoardModel's semantics
  into it (the only code that knows what a "track" is); the Gerber importer will
  emit it directly.
- **`assemble()`** turns LayerArt into a mesh — clip copper to the profile,
  derive each mask film as (outline − openings), subtract drills, extrude. It is
  format-agnostic and knows nothing of KiCad or Gerber.

A mask ArtLayer's `art` holds the OPENINGS, not the film — inverted because that
is what both formats hand us (KiCad lists which pads open the mask; a Gerber mask
file flashes the openings). Deriving (outline − openings) in `assemble()` is what
makes via tenting free.

The split was landed as a pure refactor: cx4multicart_v3 output is byte-identical
before and after (624,268 tris, same bounds), verified against the immediate
pre-refactor triangle count, and OBJ SHA-256 hashes are recorded as the baseline
for future regressions. All boards run the identical `buildLayerArt → assemble`
path — no per-board branching.

Consequence for Gerber input (not yet built): the validation checks
(`0 orphans / 0 shorts`) need nets, which Gerber lacks, so they will read n/a
rather than a false zero. But the cx4 gerbers are the SAME board as the
.kicad_pcb, so the two importers can be cross-validated at the LayerArt boundary
— copper area per layer, outline extents, opening counts — which is the strongest
correctness check available.

`BoardModel` remains the format-neutral SEMANTIC representation. It holds:

- board outline (polygon with holes)
- layer stack: copper ×N, dielectric, soldermask, silkscreen — thickness + material
- per-layer 2D geometry as polygon sets
- drills (plated / non-plated)
- component instances (transform + mesh reference)

**Tessellation is the real project, not rendering.** A Vulkan RT pipeline over
existing triangles is well-trodden. Turning aperture flashes and s-expressions
into a watertight board is where the months go.

## Known hard parts

- **Gerber layer identification.** No standard filename convention exists.
  Gerber X2 `%TF.FileFunction` attributes solve it when present; otherwise
  heuristics plus a user override UI. This is a UX problem, not just a parsing one.
- **Aperture macros** (`%AM`) are a small expression language. Real sub-parser.
- **Excellon drills** are a loose format — units and coordinate format often must
  be guessed.
- **Polarity** (`%LPD` / `%LPC`) is a boolean subtract, not a draw order.
- **KiCad zones** are a gift: `filled_polygon` is already computed in the file, so
  we never run the pour algorithm ourselves.
- **KiCad silkscreen text** uses stroke fonts (Newstroke). Text-to-geometry is its
  own sub-project. Deferred.
- **Dual GPU on this box** (RTX 5070 Ti + Radeon 8060S iGPU): device selection must
  prefer discrete and RT-capable. Never pick index 0 blindly.

## Gerber import (phase 2) — done and cross-validated

A .zip, a folder, or a .gbrjob imports to LayerArt and renders identically to the
KiCad path. Both of the founding input formats now work.

- **Parser** (`io/gerber/gerber_parser.cpp`): RS-274X / X2. Handles FS/MO, apertures
  C/R/O/P, aperture macros (primitives 1/4/5/20/21 with a `+ - x /` expression
  evaluator), regions (G36/G37), arcs (G02/G03 + G75), and LPD/LPC polarity via
  batched booleans. Unsupported features (step-and-repeat, aperture blocks) warn
  rather than mis-draw.
- **Project** (`io/gerber/gerber_project.cpp`): layer identity comes from the
  .gbrjob manifest first, then each file's `%TF.FileFunction`, then filename
  heuristics with a warning. The manifest also carries the **real stackup
  thicknesses**, which closes the "we derive the stackup" fab-truth gap for
  gerber input. Excellon drills parsed separately (see below). Board outline
  (`boardFromProfile`) unions the stroked Profile into ribbons, keeps each
  ribbon's OUTER boundary (one orientation via `Area()` sign) and **even-odd
  fills them** — so every closed loop on Edge_Cuts becomes a cutout: the
  perimeter fills solid, an enclosed mounting-hole/slot/void is enclosed twice
  and empties, an island inside a cutout three times and fills, to any depth.
  (The earlier "keep top-level outers, drop children" left every internal cutout
  filled — mounting holes weren't cut.) **Do NOT `normalizeWinding` the result:**
  it forces every path positive, flipping the even-odd holes into solid islands
  that merge into the board under assemble()'s NonZero union — the holes silently
  vanish. `assemble()` consumes the opposite-wound (negative) holes correctly.
- **Excellon drills** (`parseDrills`): round hits `X..Y..` become circles;
  **G85 routed slots** `X<x1>Y<y1>G85X<x2>Y<y2>` become obrounds (two tool-radius
  semicircle caps joined by straight sides, built with trig — no offset library).
  The G85 form is tested BEFORE the plain-hole regex, which would otherwise match
  its leading coordinate and cut a single dot at one end (the original "slots not
  cut" bug). METRIC/INCH + decimal coords; tool table from `T<n>C<dia>`.
- **Excellon ROUT mode (built 2026-07-19):** some CAM tools emit slots as
  milled paths instead of G85 — `G00X..Y..` (rapid, tool up, enters rout
  mode), `M15` (plunge), `G01`/bare `X..Y..` (linear cut; G01 is modal),
  `G02`/`G03` (arc cut, centre from `I../J..` offset or `A<radius>` with the
  ≤180° arc chosen per direction), `M16` (retract → the chain becomes one
  slot), `G05` (back to drill mode). The swept slot = the open polyline
  offset by the tool radius (ClipperOffset, round joins/ends); a plunge with
  no move is a plain hole. The bare-coordinate continuation MUST be
  intercepted before the drill-hit regex or every rout waypoint becomes a
  phantom round hole. Closed rout loops sweep to an annulus whose inner
  island cannot survive downstream winding-normalisation — the outer is kept
  and a warning says the island was cut. Malformed input (G00 with tool
  down, EOF mid-cut, unknown tool, unresolvable arc centre) closes the cut
  and warns rather than failing silently. Verified on a synthetic package
  (round hit + G85 + L-shaped rout + I/J arc rout: swept areas analytic-
  exact); cx4 (217/215) and neptune (1622) gerber regressions unchanged.

### Cross-validation: two independent paths, same board

The cx4 gerbers are the same board as `cx4multicart_v3.kicad_pcb`, so the two
importers were compared at the LayerArt boundary. After fixing the bugs below,
**every copper layer and the soldermask match to <1%**:

| layer | KiCad | Gerber | ratio |
|---|---|---|---|
| F.Cu | 294.20 | 293.22 | 1.003 |
| In1..In6 | — | — | 1.005-1.006 |
| B.Cu | 184.47 | 183.45 | 1.006 |
| F.Mask openings | 175.86 | 175.99 | 1.001 |

The residual ~0.5% is circle faceting (48-gon vs KiCad's finer circles). The
rendered boards diff at 3.4% of pixels, all at pad edges (faceting/antialiasing).

### Gerber bugs found by cross-validation (each was a real defect)

- **Macro primitive lines were silently dropped.** `%AM…%` block statements are
  split on `*` but keep their leading newlines, so a primitive line arrived as
  `"\n4,1,…"` and its first char was not a digit — the collector never fired and
  **every macro (RoundRect) aperture produced nothing**. Copper survived on its
  traces; the mask, being mostly RoundRect pads, collapsed to 45% area. This is
  why the "outer layers overhang" theory was wrong: it was this bug all along.
  Fix: trim each block statement.
- **The final image must NOT be winding-normalised.** It is the result of Clipper
  booleans, so it is already correctly wound with holes negative. Forcing all
  paths positive filled every antipad clearance in the ground planes (In5/In6
  came out 24% too big). `normalizeWinding` is for the KiCad path's soup of
  overlapping same-sign primitives, never for a finished polygon-with-holes.
- **FileFunction is case-inconsistent in KiCad itself:** a file's X2 attribute
  says `Soldermask` while the same board's .gbrjob says `SolderMask`. Compare
  case-insensitively or masks get dropped as unclassified.
- **X2 attributes come in TWO forms; parse both.** With "Use extended X2 format"
  ON, KiCad writes `%TF.FileFunction,Profile,NP*%` (an extended `%…*%` block); with
  it OFF, the SAME attribute is a G04 comment: `G04 #@! TF.FileFunction,Profile,NP*`.
  A package with no `.gbrjob` and X2-off (e.g. the COSAM Neptune export) then has
  NO recognised Profile, and the import fails with "no board outline". `word()`
  detects the `#@!` marker and routes the payload to the same handler as a `%TF`
  block, so file classification works for either export style.

### Consequence for the UI

Gerber has geometry, not semantics: no nets, pads, vias, or components. The net
and short checks need nets, so the status bar reads "gerber — no net data for
checks" rather than a green zero that would falsely imply "verified clean". The
properties dock shows what LayerArt actually carries.

## Measured facts about the KiCad format

Everything here was verified against real files, not recalled.

- **Layer ordinals are not physical order.** KiCad 10 numbers F.Cu=0, B.Cu=2,
  In1.Cu=4, In2.Cu=6. Sorting by ordinal puts B.Cu second from the top and
  silently corrupts an 8-layer stack. `copperStackRank()` ranks by name instead.
- **Footprint rotation is clockwise in raw file coordinates.** The stored angle is
  counter-clockwise as displayed, but the file's Y axis points down. Measured:
  the positive convention orphans 58 of 2512 track endpoints on
  `cx4multicart_v3`; the negative convention orphans zero. Do not "fix" the sign.
- **Pad rotation is stored ABSOLUTE; pad position is stored relative.** The two
  halves of `(pad ... (at x y rot))` do not share a frame. The position is
  footprint-local and must be transformed by the footprint's placement; the
  rotation is already a board angle including the footprint's orientation, so
  adding `comp.rotation` to it double-counts. Measured — stored pad rot always
  equals footprint rot when the pad carries no rotation of its own:

  | ref | footprint rot | stored pad rot |
  |---|---|---|
  | U1 | −90 | 270 (≡ −90) |
  | C2, R2, U3 | 90 | 90 |
  | C6, U2 | 0 | absent (= 0) |

  Consequence of getting it wrong: on U1 (TSOP-56, 0.5mm pitch, 1.575 × 0.3mm
  pads) the double-count laid each pad *along* the pitch axis, so all 28
  overlapped and unioned into one solid bar. Caught by eye, not by any check —
  see `validatePadOverlaps`, added afterwards, which scores it 127 shorts.
- **Bottom-side pad coordinates are stored pre-flipped.** Do not mirror them.
  `applyTransform` deliberately takes no mirror parameter. Measured: adding an X
  mirror for bottom footprints drops bottom-pad connectivity from 52/52 to 6/52
  and orphans 47 track endpoints. `bottom` affects layer assignment and Z, not
  the XY transform.
- **`filled_polygon` is pre-computed**, as hoped. We never run a pour.
- **Winding must be normalised before any boolean.** `FillRule::NonZero` sums
  winding numbers, so overlapping paths wound opposite ways cancel to zero and
  punch a hole exactly where they overlap — copper disappearing at every
  track/pad junction. Sources genuinely disagree. Measured signed areas on
  `cx4multicart_v3`:

  | source | area | winding |
  |---|---|---|
  | track (ClipperOffset) | +0.340 | positive |
  | pad oval (ClipperOffset) | +1.354 | positive |
  | pad circle (hand-built) | −1.756 | negative |
  | pad roundrect (hand-built) | −0.811 | negative |
  | drill circle (hand-built) | −0.070 | negative |
  | zone fill (KiCad) | −1507.894 | negative |
  | board outline (KiCad) | −1750.252 | negative |

  Cause: `toClipper()` negates Y, which reverses the winding of every path we
  build by hand — but not ClipperOffset output, which derives orientation from
  already-flipped input. `normalizeWinding()` fixes it. Switching to
  `FillRule::Positive` does **not** — that deletes negatively-wound shapes
  instead of merging them. Single shapes survive either way (|−1| ≠ 0), so this
  is invisible until two sources overlap.
- **The dielectric is SLICED, not one block — cut at inner-foil MID-PLANES so
  the slabs ABUT.** A real multilayer board is copper / prepreg / copper / core /
  copper …; the substrate is emitted as one slab between each pair of cuts, all
  named `"substrate"`, so the exploded view shows an inner trace layer in its true
  position between two dielectric slabs instead of the inner copper sliding
  through a single monolithic block. Each slab sorts to its own explode rank by
  centre Z, so it interleaves with the foils for free.
  - **Cut at the foil CENTRE, not its faces.** Cutting at the faces leaves a
    copper-thickness gap between slabs that shows as a line on the *collapsed*
    board's edge (empty, or a z-fighting copper edge). Cutting at the centre makes
    adjacent slabs share a coplanar wall — which renders seamlessly — so a
    collapsed board reads as one solid block, while each inner foil still lands on
    a slab boundary and peels out cleanly. Only foils strictly inside the core
    cut it (F/B.Cu bound it). `<2` cuts → one slab (2-layer / single / flex).
  - **Inner copper is hidden while the board is collapsed AND opaque**
    (`DrawItem::hideWhenCollapsed`, gated on `!peeling && substrateOpaque` in
    `drawFrame`). Buried copper is invisible except as a z-fighting line at the
    cut edge, so hiding it at rest is what actually makes the block look solid;
    it reappears the instant the board peels. A *translucent* substrate is the
    exception — there the whole point is to see through to the inner layers, so
    they stay shown even collapsed.
  - Consequences: `setSubstrateAppearance` must recolour **all** slabs (no early
    `break`), and the stackup tree dedupes the repeated `"substrate"` name to one
    toggle.
- **Single-copper boards (single-sided / flex) need a special core case.** The
  dielectric core normally spans between the topmost and bottommost copper foils.
  With one copper layer those are the SAME layer, so the naive "top-foil-bottom
  to bottom-foil-top" gives coreTop < coreBottom — an inverted extrusion with
  flipped normals and swapped caps, which the depth test renders as torn,
  fanning triangles. When the foils do not bracket a gap, the core instead falls
  to a solid slab from the board bottom (z=0) up to the single foil's underside
  (the slicing above yields zero inter-foil spans and falls back to this one
  slab). `assemble()` handles this. Affects BOTH import paths.
- **`(general (thickness ...))` is the FINISHED board**, not the bare core. It
  includes both soldermask films and every copper foil. Stacking copper and mask
  on top of it — as an early version did — builds a board 0.07mm too thick.
- **The dielectric height is derived, and the derivation is exact:**

      dielectric = (thickness - N*copper + - 2*mask) / (N - 1)

  On `cx4multicart_v3`: `(1.6 - 8*0.035 - 2*0.01) / 7 = 0.185714`, and KiCad's own
  gerber job file reports **0.1857** for all seven dielectrics. Copper 0.035 and
  mask 0.01 are KiCad's defaults and are confirmed by that same file. This board
  has no `(setup (stackup ...))` block, so deriving is the only option — and it
  reproduces KiCad's arithmetic exactly.
- Stackup heights live on `BoardModel`, **not** in `TessellateOptions`. They are
  fabrication facts, not render settings. Duplicating them is what produced the
  0.07mm error.

## Verification approach

Two independent invariants, because neither alone is sufficient:

1. **Net connectivity** (`validateNetGeometry`, `validatePadConnectivity`) — a
   track endpoint on net N must physically touch a pad, via, or track on net N.
   Settled the rotation sign and the mirroring question empirically.
2. **Pad overlaps** (`validatePadOverlaps`) — two pads on different nets must
   never overlap on a shared layer, because that is a short.

Invariant 1 is *blind to pad orientation*: rotating a pad's shape does not move
its centre, so the absolute-rotation bug above scored 0 orphans while merging 28
pads into a bar. Invariant 2 scores it 127 shorts. Position bugs and shape bugs
are different failure classes and need different checks.

Both are blind to anything that does not create or destroy a connection —
sub-pixel winding cancellation and copper overhanging the profile passed both
cleanly. **Only pixels catch those, and only at zoom.** Use `--preview` plus a
crop; whole-board views hide everything found so far.

Verified so far (`cx4multicart_v3`, 8 layers): every count matches the file's own
totals — 1256 tracks, 177 vias, 37 components, 266 pads (shape histogram matches
line for line), 2 zones, 217 drills = 177 vias + 40 through-hole pads, outline
stitched to 1 closed loop at 50.5 x 47.0 mm. 0 / 2512 orphaned endpoints.

Transform coverage is genuinely independent, which matters — the rotation and
mirroring questions could have confounded each other but don't:

| | rot = 0 | rot ≠ 0 |
|---|---|---|
| **top** | 31 parts | U1 (−90), R2 (90), C2 (90) |
| **bottom** | U2, U7 | U3 (90) |

Rotation is therefore exercised on top-side parts, and mirroring on unrotated
bottom parts, independently.

Format coverage: KiCad 10 (`20260206`) and KiCad 8 (`20240108`) both import clean.
KiCad 6 untested — the only file on hand is a 50-byte empty stub, which is
correctly rejected for having no layers block.

### Via type vs layer span — we follow the drill file

`cx4multicart_v3` carries a genuine data oddity, and KiCad contradicts itself on it:

- 121 of 177 vias are tagged `(via buried ...)`, but **all 121 span F.Cu..B.Cu**.
  Not one spans inner layers only. They are through vias wearing a `buried` flag.
- KiCad's **drill export** goes by *layers* → drills all 217 holes.
- KiCad's **3D viewer** goes by the *type token* → hides 121 of those holes.

The manufactured board has 217 holes. Our `.drl` count matches at 217, so we
follow layers and ignore the type token (decided with the board's author,
2026-07-16). Expect KiCad's 3D view to show fewer via holes than ours; that is
KiCad's viewer disagreeing with KiCad's own fab output, not a bug here.

`spansOuterFaces` therefore tests stack span, not the type token — which also
means a *real* blind/buried via would correctly leave the outer faces intact.
Untested: no board on hand has one.

Analysis trap worth remembering: an early check concluded "all 177 vias are
structurally identical" and missed this entirely, because the regex only matched
*list* children like `(at ...)` and `buried` is a bare atom. The check was weaker
than the claim made from it.

### Soldermask and via tenting

Mask = **outline − pad openings**. Note *outline*, not `boardArea`, and note that
drills are **not** subtracted:

- Soldermask tents *across* a hole — that is what tenting means. Subtracting
  drills makes tenting impossible and renders every via as a punched dot.
- An opening exists wherever a pad carries that mask layer. A tented via carries
  none, so the film closes over it for free. `(setup (tenting ...))` needs no
  handling at all; the layer lists already encode it.
- Any drill that should be visible already lies inside its own pad's opening.

Verified against KiCad's own mask gerbers — opening counts match exactly:

| | ours | `*_Mask` gerber flashes |
|---|---|---|
| F.Mask | 212 | 212 |
| B.Mask | 94 | 94 |

`pad_to_mask_clearance` grows each opening beyond its pad; it is 0 on
`cx4multicart_v3`, so openings are the pad shapes exactly.

Pads keep mask layers in `Pad::maskLayers`, deliberately separate from
`Pad::layers` (copper), so the copper path can never iterate mask layers.

Rendering note: bare FR4 is **tan**, not green. The green everyone pictures is
mask on top of it. Colouring the substrate green would double-count the mask and
hide where mask is genuinely absent.

### Silkscreen

Ink is clipped to the **board profile** but deliberately **not** to the mask
openings: `cx4multicart_v3` sets `(setup (subtractmaskfromsilk no))`, and the fab
honours that, so we do too. A board setting it to `yes` would need the mask
openings subtracted — not implemented, no test board has it.

Silkscreen sits **on** the mask, i.e. outside `(general (thickness ...))`. KiCad's
gerber job file lists it in the stackup with no thickness at all, so the 0.010mm
we give it is a rendering nicety, not a fab dimension. It is why Z spans
−0.010..1.610 on a 1.600mm board.

**An unfilled (`fill no`) silk rectangle/polygon must render as a hollow FRAME,
and real holes must keep negative winding.** Two bugs conspired here (a `fill no`
gr_rect came out as a solid white box): (1) `graphicPaths` stroked the closed
outline with `EndType::Joined`, which yields two SAME-wound contours — build the
frame explicitly as outer-offset + reversed inner-offset instead (like the ring
case). (2) `buildLayerArt` then called `normalizeWinding()` on the unioned silk,
flipping the hole's inner contour positive so `assemble()`'s NonZero clip filled
it solid — **do not normalise silk**; the union already yields Clipper's canonical
winding (outers +, holes −), which is what NonZero needs. Stroked **text** has no
enclosed contours (its "holes" are just uncovered space), so it rendered fine and
masked both bugs.

`fill` and `width` are independent in KiCad's model: `fill` fills the interior and
`width` strokes the outline on top. An unfilled circle with width is a ring; a
filled polygon with width is the polygon grown by half the stroke. Both are
common on silkscreen — honour both or footprint outlines come out thin and
courtyard rings disappear.

Rings rely on **opposite winding** to carve their hole, so silkscreen unions with
`NonZero` BEFORE `normalizeWinding()`. Normalising first fills every ring solid.
(Note this is the exact opposite of the copper path, where normalising first is
what stops overlaps cancelling — the two cases genuinely differ.)

`fp_rect` is expanded to a polygon at import, not stored as two corners: inside a
rotated footprint the corners rotate but "two opposite corners" can only describe
an axis-aligned box.

**Text is rendered with the embedded Newstroke font** — see
`src/text/newstroke_data.h` and `stroke_text.cpp`. Glyph strokes are centrelines,
thickened with a round pen of the item's `thickness`, exactly like a track.

**This is why pcbview is GPL-3.** Newstroke's data is GPL-2.0-or-later. See
NOTICE.md.

> Correction for the record: Newstroke is **not** SIL OFL. The OFL text inside
> KiCad's `newstroke_font.cpp` covers the Source Han Sans CJK additions only.
> Newstroke itself is GPL. Secondary sources state this wrongly; read the file
> header, not a summary.

**Text rotation is absolute and pre-normalised**, exactly like pads: position is
footprint-local and needs the transform, rotation does not. Measured — prop rot
equals footprint rot for C2/R2/U3 (90) and U7/C6/U2 (0), but U1 sits at −90 with
its reference at **+90**, because KiCad normalises text angle so it never renders
upside down. Take the stored value; never add the footprint's rotation.

**Never write `// U+5C \` in the glyph table.** The backslash glyph's own comment
ends with a backslash, which is a line-continuation — it splices the next line
into the comment, silently eating a glyph and shifting every entry after it. Line
splicing happens before comment removal, so this is invisible at a glance. The
table uses block comments for exactly this reason; MSVC's C4010 caught it.

Only Basic Latin (U+0020..U+007F) is embedded, 96 glyphs / ~2.2KB. KiCad's file
is 2.9MB because of CJK — which carries MIT and SIL OFL obligations we do not
want for glyphs no silkscreen needs.

### Copper is clipped to the board

Copper cannot exist where there is no substrate. Each layer is intersected with
`boardArea` (= outline − drills), which is computed once and reused for both the
substrate extrusion and the copper clip: `copper ∩ (outline − drills)` is exactly
`(copper ∩ outline) − drills`, so one intersection does both jobs.

This is not cosmetic. Pads are routinely drawn overhanging the profile and routed
off during fabrication. On `cx4multicart_v3` the clip is what makes **castellated
edge holes** render correctly — 16 drills sit exactly on the Y = −32.9792 profile
edge and 17 on Y = −47, at 1.27mm pitch, deliberately bisected by the board edge
(confirmed by the board's author). Unclipped, they float outside the board as
whole pads; clipped, they become the half-plated castellations they actually are.

### Geometry, verified against an independent source

The board outline was cross-checked against `cx4multicart_v3-Edge_Cuts.gm1` — a
Gerber KiCad exported from the same board, which our importer never reads. The
20 `gr_line` segments stitch into one closed staircase loop, and the extents
agree exactly: X 23.000..73.500, Y −72.000..−25.000 in both. The board really
does have those notches; they are not gaps in the substrate.

Free result: Gerber Y is negated the same way our world Y is, which phase 2 can
rely on.

Tessellation output for that board: 624k triangles, 939k vertices, 3.8s.
Z spans −0.035..1.635 — B.Cu foil, 1.6mm substrate, F.Cu foil. Inner layers are
correctly occluded inside the substrate.

Visual checks use `--preview` (software rasteriser, no GPU or window needed).
It stays useful as a headless smoke test once Vulkan lands.

### Depth is REVERSED-Z. Do not "fix" it back.

Depth runs **1.0 at the near plane → 0.0 at infinity**, cleared to **0**, compared
with **GREATER**, in a **D32_SFLOAT** buffer. The perspective projection is
**`GLM_FORCE_DEPTH_ZERO_TO_ONE` is a CMake compile definition, never a
`#define` in a .cpp** (fixed 2026-07-19). It configures glm's projection
matrices for Vulkan's 0..1 clip depth instead of OpenGL's -1..1, so it must be
set before glm is included ANYWHERE in a translation unit. It lived as a
`#define` in `vulkan_window.cpp` — until `vulkan_window.h` gained a glm include
(v1.13, for the measurement tools) and, being included on line 1, configured
glm first. `glm::ortho` then emitted OpenGL depth, Vulkan clipped everything
that mapped below 0, and **the board was sliced along a plane in orthographic
view** — the near side, the far side, or through a component, depending on
where the geometry fell. Only ortho showed it because
`infiniteReverseZPerspective` is hand-written; `glm::ortho` is the only glm
projection in the codebase. Do not move it back into a source file.

**Orthographic depth brackets the SCENE, never the eye** (fixed 2026-07-19).
A parallel projection has no viewpoint: geometry BEHIND the camera plane still
projects into the image and must still be drawn. Carrying the perspective habit
of a small positive near plane (`0.005 * distance`) clipped whatever crossed
that plane, and at a grazing angle that is the corner NEAREST the viewer — the
board was visibly sliced along a straight line as you tilted or zoomed in, and
the substrate wall at the cut went black. The ortho range is now
`distance ± sceneRadius()` (board corner + a full peel + margin), which removes
the clip and tightens depth precision at the same time (~100 mm linear range
instead of 0.09 mm → 1072 mm). The TRACERS need the mirror of this: parallel
rays start on the camera plane, so their origin is pushed back by the same
radius — moving a parallel ray's origin cannot change what it hits.

*infinite* reversed-Z (no far plane at all). Orthographic swaps near/far to match,
so both paths share one convention.

This is required, not stylistic. A PCB is a pathological depth case: features
0.010mm apart (copper 1.590, mask 1.600) viewed from ~70mm away. Conventional
0..1 depth hoards precision at the near plane, so with `near = 0.05` that gap
resolved to roughly **7 float ULPs** when zoomed out — visible flicker that
cleared as you zoomed in, because zooming in widens the ULP gap. Reversed-Z lines
the float exponent's dense region (near zero) up against the hyperbolic depth
curve's coarsest region; the two errors cancel and precision goes near-uniform.

Note this is a *different* bug from the coplanar z-fighting culling fixed. Culling
handles surfaces at *identical* z. Reversed-Z handles surfaces at *nearly* the
same z. Both were live at once, and fixing only one left the other looking like
"it's still noisy".

If depth is ever changed back to `LESS` / clear-to-1, every one of these must
change together: compare op, clear value, and both projections. Half a conversion
renders a solid depth-failed void.

The near plane also scales with orbit distance (`distance * 0.005`, floor 0.02)
rather than sitting at a fixed hair's breadth.

### Renderer/camera facts worth not relearning

- **Backface culling is correctness, not performance.** The stackup is full of
  exactly-coplanar faces: copper's top and the mask's underside both at z=1.590;
  copper's underside and the substrate's top both at z=1.555. Each pair is
  triangulated independently by earcut, so interpolated depth differs by an ULP
  per pixel and the depth test flickers — visible as speckled, torn traces.
  Culling removes the inward-facing half of every pair. Do not "restore
  two-sided rendering for safety"; that is what caused the artefact.
  extrude()'s winding is consistent and was verified by enabling culling and
  observing that nothing disappeared.
- **Lighting is camera-relative.** A world-fixed key light leaves the underside
  permanently in shadow, so the bottom view renders dark — useless for
  inspection. The key/fill rig is built from the view vector, offset up-and-right
  so facing surfaces still have falloff (a pure headlight reads flat).
  - **The key is POSITIONAL, not a fixed direction**, placed at
    `eye + (camRight+camUp)*0.55*viewDist` and pointed per-fragment. A directional
    key hits every point on a flat face at the same angle, so a large flat top
    (an IC lid) renders as one dead tone; a positioned lamp's incidence sweeps
    across the face, giving a soft diffuse gradient. Scaling the offset with view
    distance keeps the falloff looking the same at any zoom. A small Fresnel rim
    (`pow(1-n·v, 4)*0.08`) adds a grazing sheen so curved bodies and silhouettes
    gradient toward the edge and dark parts lift off the background.
- **The camera basis comes from YAW ALONE.** `right = (cos yaw, sin yaw, 0)` is
  horizontal by construction, always unit, always perpendicular to forward, and
  never degenerate — so pitch wraps through the poles indefinitely and the view
  inverts past vertical, as it should. `glm::lookAt(eye, target, worldUp)`
  collapses when looking straight down, which is why an earlier version clamped
  pitch to ±89° and rotation "hit a wall". If you reintroduce lookAt, the wall
  comes back.
- **Pan uses the camera's right/up, not world Z.** With world Z as "up", a
  vertical drag pushed the board through its own thickness instead of across the
  screen.

### Exploded view — a staged peel, not a linear ramp

Ctrl + scroll peels the stackup **outside-in, one ring at a time**, dwelling
between stages so you can stop and look at each layer as it comes off. A linear
ramp (everything sliding at once, proportional to rank) was tried first and is
worse: layers smear past each other and there is no natural place to stop.

The model:

- A part's **rank** is baked into the material table (`params.z`) at upload,
  derived from where its geometry actually sits in Z — not from layer names, so
  it is right for any stackup and handles the substrate and masks, which have no
  stack index. Ranks are signed and centred; rank 0 never moves and anchors the
  board.
- Its **ring** is `|rank|` — distance from the mid-plane. Ring `maxRank` (the
  outermost) starts moving at progress 0; each ring inwards waits one more stage:

      travel(ring) = max(progress - (maxRank - ring), 0) * mmPerStage

  Once a ring starts it never stops, so outer layers keep flying away while the
  next lifts.
- The **dwell** lives in `easedExplodeProgress()` on the CPU, not the shader:
  within each stage, move (smoothstepped) for the first 62%, then hold at exactly
  the stage boundary. A dwell is therefore a genuinely stationary stack, not a
  slow crawl. ~3 wheel clicks per stage: two to lift, one to hold.
- **Progress is animated, not set directly.** A wheel click is a discrete jump of
  ~0.34, so writing it straight to the shader teleports the layers no matter how
  nicely the stages are eased — the per-stage easing shapes *where* a layer goes,
  not *how it gets there*. `setExplodeProgress()` sets a TARGET;
  `stepExplodeAnimation()` eases toward it with an exponential approach
  (~0.07s time constant, framerate-independent, eases out for free).
  `snap=true` for startup and board reload — a board should not peel itself open
  on load.
- The animation is driven **from `render()`**, not a QTimer: rendering is on
  demand, so the animation drives the frames and the frames drive the animation.
  `render()` re-arms `requestUpdate()` only while still moving, so the loop stops
  and the GPU idles once it settles. `dt` is clamped to 0.1s so a stall does not
  teleport the stack — the whole point is that it never jumps.
- Cost is **one push constant per frame**. Geometry and ranks never change.
- Rank is indexed by MATERIAL, not by `mesh.parts` — empty parts are skipped at
  upload, so the two indices diverge.

**The substrate fades as it peels.** The laminate is precisely what hides the
copper, so an exploded view of an opaque substrate shows you slabs of FR4. It
goes from fully solid at rest to ~0.42 alpha at a full peel, driven by
`push.params.w` (normalised progress) and gated by a per-material flag
(`material.params.w`) so only the substrate does it. At peel 0 the alpha is
exactly its authored value, so a collapsed board is unchanged.

**That forces the substrate into the blended pass, which forces sorting.** The
blend pipeline does not write depth, so translucent surfaces cannot self-sort: a
nearer one drawn first gets composited *under* a farther one.

Sort by **Z-stack order relative to the camera, NOT eye-distance to centroid.**
The board's translucent layers (mask, substrate) span the same X/Y and differ
mainly in Z, so a 3D distance sort is dominated by the near-equal X/Y terms and
flips order as the camera orbits — which reads as the top surface flickering
between mask-green and substrate-colour. Ordering purely by peeled Z, with the
direction chosen by which side of `boardMidZ_` the eye is on, is stable and flips
only when the camera crosses the board plane (correct: from above you see the
mask, from below the substrate). The peel `travel()` must still mirror
`board.vert`'s exactly. A handful of parts — noise next to 592k triangles.

### State set before first expose is silently dropped — this has bitten 3 times

The renderer is only created when the window is **first exposed**. Anything
configured during `MainWindow` construction runs against a null renderer and is
lost. Every one of these was a real bug:

1. the mesh pointer,
2. explode distance (toolbar read "Explode 4.00 mm" over a flat board),
3. explode progress — worse, because `setExplodeProgress` *clamped* against
   `renderer_->maxRank()`, which is 0 before upload, so the value was silently
   zeroed rather than merely dropped.

Rules: never clamp against renderer state when the renderer may not exist, and
re-apply everything from `initialise()` after creating it. Any new render setting
needs the same treatment — assume it will be configured before first expose,
because that is when `MainWindow` is built.

### No console window, but the CLI still prints

Built `/SUBSYSTEM:WINDOWS` (with `/ENTRY:mainCRTStartup`, so `main()` stays the
entry point) — a GUI should not drag a black console along. `attachParentConsole()`
in main.cpp then attaches to the **parent** console when there is one: run it from
a terminal and output appears there; double-click it and `AttachConsole` simply
fails, which is exactly the GUI case.

**The guard in that function is load-bearing.** If stdout is already connected —
redirected to a file, or piped by a shell — reopening `CONOUT$` rips the
redirection away and sends output to the console instead, so `pcbview board >
out.txt` silently produces an empty file. Only claim the console when nothing
else owns stdout. This was caught by testing all three paths (pipe, file
redirect, bare terminal) rather than just one.

`#define NOMINMAX` before `<windows.h>`: it defines `min`/`max` as macros that
collide with `std::min`/`std::max`.

Known quirk, not our bug: PowerShell's `*>` redirection does not wait for a
GUI-subsystem process, so it can produce an empty file. Pipes and true OS-level
redirection both work.

### Board appearance (thickness override + substrate material)

Render ▸ Board appearance lets you override the finished thickness and set the
substrate colour/opacity live — for previewing a design at a different fab
thickness, or making a "flex" render look like amber polyimide.

- **Thickness override** re-derives the whole Z stack via `applyThickness()`,
  which models what a fab actually varies: copper foil and mask film stay fixed,
  the dielectric flexes to hit the target. Only Z moves, so it re-runs
  `assemble()` (fast — the parse/buildLayerArt is the expensive part, and that is
  cached in `MainWindow::baseArt_`). Works for both import paths.
- **Substrate colour + opacity** re-write only material 0 in the bindless table —
  no re-tessellation — so a colour picker / slider drives it live. Opacity < 1
  moves the substrate into the blended pass PERMANENTLY (not only while peeling),
  which the depth-sorted blend pass already handles. The flex-cable look is a
  bottom view of a single-sided board with a thin, ~40%-opacity amber substrate:
  the copper reads straight through it.

Both settings live in `MainWindow` (`thicknessOverride_`, `subColor_`,
`subOpacity_`), NOT only in the renderer — because the renderer does not exist
until first expose and resets the substrate to default tan on every
`uploadBoard`. They are (re)pushed from the `boardUploaded` signal. This is the
same before-first-expose trap that has bitten explode, mesh, and now this.

- **Soldermask colour** is a separate control (`setMaskColor`, both F and B mask
  parts) — blue/red/black mask etc. Opacity stays at the film default.

Headless verification hooks (dialog can't take synthetic input):
`PCBVIEW_THICKNESS=0.3`, `PCBVIEW_SUBSTRATE=r,g,b,opacity`, `PCBVIEW_MASK=r,g,b`.

### GUI notes

- **Qt owns the window; we own Vulkan.** Deliberately *not* `QVulkanWindow` —
  that class creates the `VkDevice` itself, which would take the ray tracing
  extensions out of our hands and break RT-readiness rule 1. Instead
  `QVulkanInstance::setVkInstance()` takes *our* instance and we ask Qt only for
  a `VkSurfaceKHR`. Device, buffers and pipelines are untouched by the GUI.
- **Include order is load-bearing:** `<vulkan/vulkan.h>` must precede any Qt
  header. Qt's Vulkan headers define `VK_NO_PROTOTYPES` so Qt can route through
  its own loader; include Qt first and every direct `vkFoo()` call fails to
  compile.
- **Teardown order is load-bearing:** Qt destroys the platform window — and with
  it the `VkSurfaceKHR` — before `~VulkanWindow` runs. The swapchain must die
  first, so `releaseResources()` is driven from
  `QEvent::PlatformSurface`/`SurfaceAboutToBeDestroyed`, not the destructor.
- **The scene renders offscreen at `renderScale_`, then blits to the swapchain;
  the UI draws after, at native resolution.** That ordering is the whole point —
  the board can be supersampled without the text going soft.
- Swapchain images need `TRANSFER_DST` (scene blitted in), `TRANSFER_SRC`
  (capture) and `COLOR_ATTACHMENT` (UI pass).
- Rendering is **on demand** (`requestUpdate`), not a continuous loop. A viewer
  that is idle 99% of the time should not pin a GPU. Consequence: the perf
  readout only ticks while you interact.
- **View changes ANIMATE.** `setViewTop/Bottom/Iso` and `frameBoard()` set a
  `viewTarget_` Camera and let `stepCameraAnimation()` ease `camera_` toward it —
  the exact on-demand-driven, exponential-approach pattern as the peel
  (`stepExplodeAnimation`), so the render loop stays alive only while moving. Yaw
  takes the shortest way round (target wrapped to within ±π of current). A board
  load/reload passes `snap=true` so it doesn't swoop on open; a mouse grab clears
  `cameraAnimating_` so the drag never fights the glide.
- **Screenshot + print reuse the file-capture path.** `MainWindow::grabFrame()`
  asks the renderer to write the next presented frame (`requestCapture`), then a
  one-shot `frameRendered` connection loads that BMP as a `QImage`. Screenshot
  saves it; print (`Qt6::PrintSupport`) shows a **`QPrintPreviewDialog`** whose
  `paintRequested` paints the image to the page. Three modes: as-shown; flat (snap
  to top-orthographic, capture, restore camera); and flat at **1:1** —
  orthographic mm-per-pixel is `2*halfW/width`, scaled to the printer's DPI so
  the board prints at true physical size.
  - **`halfW` comes from `VulkanWindow::orthoHalfHeight()`, and MUST.** That
    one function defines the orthographic extent for the raster projection,
    the tracers' ray spans and this print scale. It returns
    `distance * tan(fov/2)` — the perspective frustum's half-height at the
    orbit distance — so toggling Ortho does not change the board's on-screen
    size. It was `distance / 2` until 2026-07-19, an arbitrary constant that
    made the O key jump the zoom by 21% (measured 1337 px vs 1097 px across
    an identical camera). The print path had its OWN copy of that constant,
    so fixing only the projection would have silently mis-scaled every 1:1
    print by the same 21% — do not re-inline this value.
  - **High-resolution export** (`grabFrame(then, exportScale)`): raise the
    internal render scale, capture the OFFSCREEN scene image instead of the
    swapchain (`requestCapture(path, sceneTarget=true)`), restore the scale.
    The whole feature is those three steps because the internal-resolution
    slider already renders the scene off-screen at a multiple of the window —
    a 4× export is 16× the pixels of a window grab, genuinely re-rendered.
    Two things it must do: **wait for convergence** before grabbing (raising
    the scale restarts accumulation, so a traced mode would otherwise export
    one noisy sample — grabFrame polls `accumulating()` with a frame budget),
    and **draw the overlay into the scene image** at export size, or the
    measurements and dimension callouts silently vanish from the export. The
    overlay vertices are in WINDOW pixels and its push constant stays the
    window size; only the VkViewport becomes the export extent, so the
    viewport transform does the scaling and stroke widths grow with it.
    Headless hook: `PCBVIEW_CAPTURE_SCENE=1` alongside `PCBVIEW_CAPTURE`.
  - **The grabFrame callback MUST be deferred** (`invokeMethod(..., QueuedConnection)`).
    `frameRendered` is emitted from inside `render()`; opening a modal dialog
    there re-enters the render/event machinery and left the print preview blank.
    Running the callback on the next event-loop turn fixes it.
- **Panels are `CollapsibleDock` (VS-style auto-hide).** Qt has no native
  auto-hide dock, so `collapsible_dock.h` builds one: a `QStackedWidget` content
  (page 0 tree, page 1 a thin spine), a custom title bar with pin + hide buttons.
  Hide **unpins and collapses** (entering auto-hide mode), so a hover-reveal is
  temporary — it re-hides when the cursor leaves — until the user explicitly
  clicks pin to keep it open. Hovering/clicking the spine expands. Leave-detection
  **polls `QCursor::pos()` against the dock's GLOBAL rect** on a timer, not
  `leaveEvent()`
  — child widgets (the tree) eat enter/leave, so `leaveEvent` fires while the
  pointer is still inside the panel. No `Q_OBJECT` (only virtual overrides +
  lambda connects), so it stays header-only. `\` still hides both docks outright.

### Synthetic keyboard input does not reach the GUI when the screen is locked

`SendKeys` (and any synthetic input) cannot cross from an automation process to
an application on the Default desktop while the workstation is locked — the input
desktop is Winlogon. This fails **silently and positively**: the app keeps
rendering its default view, the screenshot looks plausible, and the test
"passes". Ctrl+O works fine for a human at an unlocked machine.

This produced a false claim: a "bottom view" screenshot that was really the
default elevated view, presented as evidence that bottom-side lighting worked. It
proved nothing. It also caused three rounds of chasing a File→Open bug that did
not exist — the keypress simply never arrived.

**Use `PCBVIEW_START_VIEW=top|bottom|iso` instead.** It sets the opening camera
with no input synthesis, so view-dependent rendering can be verified honestly.
Add a similar hook rather than reaching for SendKeys again.

Corollary: if a UI test passes, ask what it would have looked like had the input
never arrived. If the answer is "the same", the test proves nothing.

**Even better than an external screenshot: `PCBVIEW_CAPTURE=<out.bmp>`.** It
grabs the *actual presented Vulkan frame* (via `Renderer::requestCapture`) after
a settle delay and quits — lock-independent, DPI-independent, and it captures
what the GPU rendered rather than what a screen scraper saw. This is how
component rendering was verified (top iso / explode / bottom). Combine with
`PCBVIEW_OPEN`, `PCBVIEW_START_VIEW`, `PCBVIEW_START_EXPLODE`, `PCBVIEW_THICKNESS`.

**Launch headless with `Start-Process -Wait`, not the PowerShell `&` call
operator.** `&` does not block on a windowed-subsystem app, so a follow-up
`Stop-Process` (or the next loop iteration) can kill it mid-load — which looks
exactly like a nondeterministic startup crash but is a harness artifact.

### Environment variables (headless + diagnostics) — full reference

Every hook is read once at startup / board load; none affects a normal
interactive run. All are opt-in. Grouped by purpose:

**Load & camera**
- `PCBVIEW_OPEN=<path>` — load a board (.kicad_pcb, .zip, folder, .gbrjob) the
  CLI arg can't express; the honest way to drive a headless load.
- `PCBVIEW_START_VIEW=top|bottom|iso` — set the opening camera without input
  synthesis (see the SendKeys warning above).
- `PCBVIEW_START_EXPLODE=<progress>` — open with the stack peeled to `progress`
  (0 = collapsed; up to `maxRank`, ~mid+1 with components), snapped not animated.
- `PCBVIEW_START_ORTHO=1` — open in orthographic projection.
- `PCBVIEW_START_DISTANCE=<mm>` — opening camera distance (overrides fit).
- `PCBVIEW_START_ROLL=<radians>` — opening camera roll (right-drag vertical
  can't be synthesised).
- `PCBVIEW_START_YAW` / `PCBVIEW_START_PITCH=<radians>` — explicit orbit
  angles for views the three presets cannot express. A grazing pitch is how
  the board EDGE gets inspected, which is where the orthographic near-plane
  clip was finally reproduced.
- `PCBVIEW_MEASURE=x1,y1,z1,x2,y2,z2` — pin a measurement between two world
  points in mm (mouse picks can't be synthesised).
- `PCBVIEW_NET=<name>` — highlight a net by name (the picker dock and board
  clicks can't be synthesised). KiCad boards only.

**Appearance (mirror the Appearance dialog, which input synthesis can't drive)**
- `PCBVIEW_THICKNESS=<mm>` — finished-board thickness override.
- `PCBVIEW_SUBSTRATE=r,g,b,opacity` — substrate colour + translucency (0..1 each).
- `PCBVIEW_MASK=r,g,b[,opacity]` — soldermask colour, optionally its opacity
  (drives both the raster blend and the path tracer's show-through).

**Render mode / GPU**
- `PCBVIEW_RT=1|0` — force ray-traced raster shadows+AO on/off (else persisted).
- `PCBVIEW_PT=1|0` — force path-traced mode on/off (else persisted).
- `PCBVIEW_OIDN=1|0` — force neural denoise on/off (else persisted, default on).
- `PCBVIEW_PT_SPP=<n>` — path-tracer sample cap (convergence target).
- `PCBVIEW_GPU=<name-substring>` — pick the GPU by name substring (else the
  persisted `gpuName`, else discrete+RT-ready). Matching the lavapipe device
  name selects CPU rendering.
- `PCBVIEW_RENDER_SCALE=<0.25-1>` — headless hook for the internal-resolution
  slider (some artifacts, e.g. OIDN tiling seams, only appear above a
  pixel-count threshold the default headless window never reaches).
- `PCBVIEW_FAST_MOVE=1|0` — force fast movement (reduced-res while the camera
  moves) on/off; else persisted **per device class** (`fastMovementCpu` /
  `fastMovementGpu`, default on for the CPU renderer, off for a GPU).

**Capture / verification**
- `PCBVIEW_INFER_NETS=1` — derive pseudo-nets from copper connectivity on
  load (the Nets-panel button, which a capture harness cannot click).
- `PCBVIEW_START_PT=1` — enable path tracing at startup. PT is a menu toggle the
  capture harness cannot reach, so anything PT-specific (net chase, emission,
  denoiser) is unverifiable without this.
- `PCBVIEW_CAPTURE=<out.bmp>` — grab the presented Vulkan frame after a settle
  delay, then quit.
- `PCBVIEW_CAPTURE_DELAY_MS=<ms>` — override the pre-grab settle delay (default
  1500); raise it so the path tracer can converge + denoise before the grab.
- `PCBVIEW_CAPTURE_SCENE=1` — grab the offscreen scene image (the internal
  render scale) rather than the swapchain: the high-resolution export path,
  which the Save Screenshot dialog cannot be driven into headlessly. Combine
  with `PCBVIEW_RENDER_SCALE`.
- `PCBVIEW_GPU_REPORT=<file>` — write the chosen device + ray-query/OIDN state to
  a file (console output is uncapturable on the Windows subsystem).
- `PCBVIEW_ART_DUMP=<file>` — write LayerArt geometry stats (outline/drills/layer
  path counts, areas, bounding boxes). Surfaced the G85-slot and mounting-hole
  bugs by making "holes not cut" a measurable number, not a pixel judgement.
- `PCBVIEW_VK_LOG=<file>` — capture Vulkan validation-layer output to a file.

**Components (KiCad only)**
- `PCBVIEW_NO_COMPONENTS=1` — skip 3D component bodies.
- `PCBVIEW_KICAD_CLI=<path>` — override the `kicad-cli` used to tessellate STEP
  models into the cached GLB.

**Effects (stylised)**
- `PCBVIEW_FX_COMPONENT=<0-100>` — component reflections (mirror finish).
- `PCBVIEW_FX_PADS=<0-100>` — pad/copper shine.
- `PCBVIEW_FX_SHADOW=<0-100>` — path-tracing shadow softness (sun angular
  size; slider value 15 = default = 1.2° radius, 100 = 8°).

### Verifying the GUI on a scaled display

Qt reports `devicePixelRatio = 2.00` on this machine. Any screenshot tool must be
DPI-aware (`SetProcessDPIAware()`) or Windows hands it *virtualised* window rects
and it silently captures only the top-left quadrant — which looks exactly like
"the right dock and status bar are missing and the board is off-centre". That was
a false alarm that cost real time. Measure the layout (`QDockWidget::geometry`)
before believing a screenshot.

### Fab-truth gaps (ranked by the governing principle) — TO FIX

**These are a work list, not accepted behaviour.** The governing principle is
"render what the fab will build", so every entry here is a place the render is
knowingly wrong and should eventually be corrected. Ranked by how far each one
departs from fab output. Items 1-3 also appear under "Not yet verified" below,
where the gap is a *missing test board* rather than missing code.

Things we currently render differently from what the plant would produce:

1. ~~Explicit `(setup (stackup ...))` is not read on the KiCad path.~~ **FIXED
   2026-07-20.** `buildLayerStackFromBlock` walks the block top-down and places
   every film at its own thickness; the derivation remains as the fallback for
   boards with no block. This mattered because a single derived dielectric
   height spreads an asymmetric stack evenly and puts every inner foil where
   the fab will not: on a 0.1/1.0/0.1 test stack the derived In1.Cu landed at
   z=0.880 against a true 1.180. The block is authoritative for POSITION; if
   its films disagree with `(general (thickness))` the stackup wins and the
   difference is warned about rather than silently reconciled.
2. ~~Oval drills are approximated as round.~~ **FIXED 2026-07-20.** An oval
   drill is a SLOT -- routed, not drilled -- and is now built as a stadium via
   the same `ovalPath` helper oval PADS use, so a slot and its pad share
   geometry. Plated slots get a matching stadium wall. Measured on a 1.0x3.0
   fixture: 2.783 mm2 against an analytic 2.785, where the round approximation
   removed 7.07 mm2 -- two and a half times too much material, in exactly the
   place (a mounting slot, a locating pin) where the size is load-bearing.
3. ~~Custom / trapezoid pads fall back to the bounding rect.~~ **FIXED
   2026-07-20.** Trapezoids reproduce KiCad's corner formula exactly (the delta
   skews opposite edges). Note the area is UNCHANGED by the delta, so only the
   SHAPE differs -- a 2.0x4.0 pad with delta 2.0 spans 4.0 mm, not 2.0.
   Custom pads union the anchor with every drawn primitive (poly/line/rect/
   circle/arc, filled or stroked, strokes offset by half their width like
   tracks). The old comment claimed the fallback over-reported copper; it
   UNDER-reported whenever a primitive reached beyond `size`, which is the
   normal case -- a fixture pad measured 1.0 mm2 against a true 2.785.
4. ~~No surface finish distinction.~~ **FIXED 2026-07-20.** Exposed copper was
   never "bare" -- it was hardcoded gold, so a HASL board rendered as ENIG.
   `(copper_finish ...)` now drives colour AND roughness (`classifyFinish`):
   bare/OSP salmon and matte, HAL neutral solder and visibly rougher (it is a
   dipped finish), immersion silver/tin bright, ENIG and anything unrecognised
   gold. Gerber packages have no finish to read -- a .gbrjob does not record
   one -- so they keep the gold default rather than guessing.

(The old entry 4, "No silkscreen", was stale: silkscreen with Newstroke stroke
text has been rendered since phase 1.5.)

### Not yet verified — TO FIX (needs a test board, not just code)

- **Rotations other than 0 / ±90.** No test board has one. Until one exists
  the rotation convention (see the pad-rotation note above) is unproven for
  arbitrary angles, and that convention is exactly the kind of sign error
  that hides until a 30-degree footprint shows up.
- **Custom / trapezoid pad shapes.** Not implemented; fall back to the bounding
  rect and emit a warning. No test board uses them.
- `validatePadConnectivity` is meaningless on an unrouted board — CPS3brd1 has
  zero tracks, so it reports top 0/79. Read the top-vs-bottom *gap*, not the rate.

## Measurement tools (built 2026-07-19)

Press `M` (or View → Measure distance): click two points, get a fab-exact
distance. View → Board dimensions draws width/height callouts. How it hangs
together, and why:

- **Snap-first picking, no ray tracing.** `BoardMesh::snapPoints` carries
  every drill/bore centre (top AND bottom face, so either viewing side
  snaps), every pad centre (`LayerArt::padCentres`, KiCad only — a Gerber
  flash is indistinguishable from any other exposure), and every outline
  vertex. The cursor snaps to the nearest snap point within 14 px
  (screen-projected); only a miss falls back to unprojecting the cursor
  through the inverse of the frame's own viewProj and intersecting the
  board-top plane (`BoardMesh::boardTopZ`). Snapped measurements are the
  true design dimension — hole-to-hole on the synthetic test board reads
  exactly 10.000 mm.
- **Click vs drag:** a left press in measure mode becomes a pick only if the
  cursor moves under 4 px before release, so orbit/pan/zoom stay fully live
  while measuring. Esc clears the measurement; M leaves the mode.
- **Overlay rendering is in-renderer, not Qt.** A transparent QWidget cannot
  reliably paint over a native `createWindowContainer` window (airspace), so
  `Renderer::setOverlay` takes a screen-space triangle list (pixels,
  top-left origin; 6 floats/vertex xy+rgba) drawn by a tiny pipeline in the
  existing UI pass — native resolution, alpha-blended, after the scene blit,
  so it works over raster, RT, PT, and on the CPU device, and appears in
  `PCBVIEW_CAPTURE` output. Host-visible vertex buffer per frame in flight
  (same race reasoning as cpuStaging_). Thick lines, arrowheads/ticks,
  markers, and TEXT all arrive as triangles: text is Newstroke
  (`text::layout`) — its KiCad-sense Y-down output IS screen-pixel space, so
  glyph polylines just get thickened into quads. Labels draw twice (1 px
  black offset, then white) for contrast on any board.
- **World anchoring:** `VulkanWindow::buildOverlay()` rebuilds the triangle
  list every frame from the stored `lastViewProj_`, so measurements and
  dimension callouts stay glued to the board through orbit/zoom/explode.
  Dimension callouts use `BoardMesh::outlineMin/Max` (outline bbox —
  component overhang must not inflate the board size).
- **Net panel:** pad/via snap points carry a net index
  (`LayerArt::netPoints`, emitted FIRST so they win snap ties against their
  netless drill twins -- the search keeps the first of equally-near points).
  When both measurement endpoints resolve to the same net, a corner panel
  shows the **routed PATH between the two points** -- `netPathLength()`, a
  Dijkstra walk over that net's track segments (`BoardMesh::netSegments`;
  nodes = endpoints quantised to 1 um, which IS KiCad connectivity; layers
  deliberately ignored -- same-net layer changes go through a via at the
  shared endpoint; query points attach to nodes within 1 mm) -- plus the
  net's total routed length and via count. A net joined only through a zone
  pour has no track path and says so. **Zone-poured copper is in neither
  number** -- a pour-fed power net legitimately reads short. Vias are
  counted, not measured (barrel height is stackup noise against trace
  lengths). Verified: synthetic board reads path 13.090 / total 24.495 mm,
  both analytic-exact, against a 10.000 mm crow-flies line.
- **Toolbar buttons share the View menu's QActions** (ruler = measure,
  speed square = dimensions), so button, menu checkbox and the `M` key stay
  in sync with no duplicated state. Both icons are **drawn with QPainter**
  (`rulerIcon()` / `speedSquareIcon()`) rather than shipped as bitmaps:
  crisp at any DPI or icon size, themed via `theme::kText`, and the asset
  list stays down to the app icon. The toolbar uses
  `Qt::ToolButtonTextBesideIcon`, so the older icon-less buttons still show
  their labels.
- Headless: `PCBVIEW_MEASURE=x1,y1,z1,x2,y2,z2` pins a measurement (mouse
  picks cannot be synthesised) and resolves endpoint nets exactly like a
  snapped click; the `dimensionsOverlay` setting persists the callout
  toggle. Verified: two V3V3 vias on cx4 read 30.955 mm crow-flies (exact)
  with "Net V3V3, Routed 28.112 mm, 26 vias" in the panel.
- **Verifying menus/toolbars** (they never appear in `PCBVIEW_CAPTURE`, which
  grabs the Vulkan frame): grab the window with `PrintWindow(hwnd, dc, 2)`
  (`PW_RENDERFULLCONTENT`) after `SetProcessDPIAware()` — without the DPI
  call the rect comes back virtualised on this 200% display and only the
  top-left quadrant lands. That is how the toolbar icons were checked.

## Net highlighting (built 2026-07-19; Gerber X2 nets added 2026-07-20)

Pick a net and it lights up across every layer while the rest of the board
mutes, so a signal can be followed through the stack and the exploded view.
Sources: the NETS dock (filterable list), a click on the board, or
`PCBVIEW_NET=<name>` headlessly. All three funnel through
`MainWindow::highlightNet`, so the list selection, the renderer and the status
readout can never disagree.

- **Net identity survives the copper union.** A copper layer is ONE merged
  polygon set, which destroys the per-track net. `buildLayerArt` now
  accumulates each layer's copper **per net** as well as in bulk
  (`ArtLayer::netArt`), and `assemble` runs the layer's clip once per net
  rather than once per layer. That is lossless — design rules keep nets
  apart, so unioning each separately covers the same area — and was verified
  by triangle count: cx4 tessellates to **810,092 triangles, identical to the
  pre-change binary, every per-layer count matching**.
- **Per TRIANGLE, not per part.** Splitting copper into a part per net would
  multiply draw calls by the net count. Instead `Part::triNet` rides along to
  a global `triNetBuffer_` in the same order as the index buffer, exactly like
  the existing per-triangle material buffer.
- **The raster fragment shader only gets `gl_PrimitiveID`**, which restarts at
  zero every draw. `MaterialGpu::extra[0]` carries the draw's first global
  triangle so the shader can rebase it. That field is why the `Material`
  struct had to change in **all four** shaders that read the material SSBO --
  board.vert, board.frag, board_rt.frag AND pathtrace.comp -- or they read at
  the wrong stride.
- **Bloom** (`recordBloom`) gives the glow its aura. It runs on the FINISHED
  scene image, before the blit, so one implementation covers raster, RT, path
  tracing and high-resolution exports alike. Two passes: threshold+downsample
  into a half-resolution target, then add it back with additive blending
  (sampling a different image than it writes, which is what keeps it legal).
  **Half resolution, not quarter** — a highlighted trace is a pixel or two
  wide and a quarter-res downsample attenuates it to nothing before it can
  bleed. It runs only while a net is highlighted, since that is the only case
  with pixels deliberately pushed past white.
  - Two validation-layer findings worth keeping: `gl_PrimitiveID` in a
    FRAGMENT shader declares the SPIR-V **Geometry** capability, so the
    device must enable `geometryShader` even though no geometry shader
    exists (NVIDIA tolerated it without; a stricter driver would fail to
    create the module). And the net colour/light buffers are referenced by
    live descriptor sets, so they need a `vkDeviceWaitIdle` before being
    freed on a highlight change.
- **`LayerArt::warnings` vs `LayerArt::notes` mean opposite things.** A
  WARNING is something that may make the render disagree with the board (a
  file we could not identify, a thickness we had to guess). A NOTE is a file we
  identified correctly and deliberately did not draw: solder paste is a stencil
  aperture, a drill map documents the board rather than being part of it. Both
  are shown under View > Import report and both go to stderr, but only warnings
  raise the status line. A viewer that announces five warnings on a healthy
  package teaches the user to ignore it, and then the one warning that matters
  goes unread.
  - Layer kinds must be honest about this too. `Kind::Documentation` exists so
    a drill map -- which declares itself with `TF.FileFunction,Drillmap` -- is
    not reported as unidentified. It told us exactly what it was.
  - **Manifest and in-file spellings differ.** A `.gbrjob` says `SolderPaste`
    and `SolderMask`; a file's own X2 attribute says `Paste` and `Soldermask`.
    Case-folding hides the mask case by accident; paste needed both spellings
    listed, and any NEW kind must be checked against both sources.
- **Pseudo-nets (`geom/connectivity.cpp`) recover connectivity from copper
  alone**, for packages with no net data. Per layer, the copper is unioned
  through a `PolyTree64` so nesting is explicit -- each top-level outer contour
  is an island and its children are that island's holes; a flat `Paths64` could
  not tell "a second island" from "a hole in the first" and would merge a pour
  with whatever sits in its cutout. A grandchild contour is copper sitting
  inside a hole (a pad in a pour cutout), so it becomes its own island.
  Union-find then joins islands through `barrels` (plated, whole-stack --
  unplated holes are absent from `barrels` by construction, which is exactly
  right since they conduct nothing) and through `partialBores` across only the
  layers in their span.
  - Report `connecting` alongside `groups`. A dense board yields ~1900 groups
    of which ~1200 are single isolated islands (lone pads, pour fragments,
    copper logos); quoting the total alone reads as an explosion when most of
    what was found is isolated by design.
  - Derived nets have NO routed length -- there are no track centrelines to sum
    -- so the panel shows copper area instead. Printing `0.0 mm routed` would
    be inventing a number rather than measuring one.
  - They are ordered by area and the panel does NOT sort them by name, because
    `~1, ~10, ~100` is worse than useless. Real nets still sort by name.
- **Gerber nets come from X2 `%TO.N%` object attributes**, not from a
  schematic (Gerbers have none). The attribute names the net for every object
  drawn until it is replaced or `%TD*%` deletes it, so the parser tracks it as
  state and tags geometry in `addDark` -- the single funnel all dark geometry
  passes through -- BEFORE compositing, because `image_` is one merged union in
  which per-object identity is gone. Linear draws additionally become
  `netSegments` graph edges (and routed length); arcs and flashes contribute
  area but no edge, and region fills are excluded because a zone outline is not
  a route. Clear-polarity objects are never tagged: a thermal relief belongs to
  no net, and subtracting it would misreport a relief as a break.
  - The net index must be GLOBAL to the board, not per file -- a net appears on
    several layers, and highlighting, the Nets panel and the measure tool all
    key off one identity.
  - Packages without `TO.N` are unaffected: the net list is empty and every
    region stays in the -1 bucket, exactly as before.
- **The chase animation runs at DISPLAY time in the path tracer**, and this is
  the crux of the whole feature. pcbview's PT is an offline-style progressive
  accumulator: it averages N samples of a frozen scene and resets on any
  change. Animating the scene would reset convergence every frame, so a
  highlighted net would render as one noisy sample forever. Instead the net is
  traced ONCE at full emission while `pathtrace.comp` writes a first-hit AOV
  (`ptNetPhase_`: .r = position along the net, .g = 1 on highlighted copper),
  and `tonemap.frag` modulates those pixels as it resolves. Repainting re-runs
  only the display resolve — `resetAccumulation()` is never called — so a
  converged image animates.
  - Games solve the general form of this differently: 1 spp plus reprojection
    with motion vectors, ReSTIR reservoir resampling, and an ML denoiser
    (NRD/DLSS-RR). That is a real-time architecture; it would be a rewrite
    here, and pcbview's PT earns its quality precisely by *not* being one.
  - **Known limitation, by construction:** only copper the camera sees
    DIRECTLY animates. The red spill onto surrounding copper and laminate is
    baked into the accumulated radiance and cannot be unbaked per pixel, so it
    stays steady while the band travels. It reads as a moving filament inside a
    stable glow, which is defensible, but it is not what a physically moving
    emitter would do.
  - `netChase()` is duplicated in `board.frag`, `board_rt.frag` and
    `tonemap.frag` and MUST stay in step. The per-triangle net buffer is
    likewise read by three shaders under two different array names (`tris` and
    `triNet`) — which is exactly how the path tracer got missed when the buffer
    grew a phase field, silently reading 8-byte entries as 4-byte ints and
    lighting up unrelated copper. Grep for the binding, not the name.
- **The highlight is EMISSIVE, in every mode.** The net is red — copper is
  gold and laminate green, so red is the one hue that cannot be mistaken for
  either — and it is emitted rather than shaded:
  - *Raster / RT*: the fragment shader outputs the glow colour directly and
    returns before any lighting. Running it through the rig instead let the
    white specular and environment terms wash the red out to salmon, and put
    the trace back in shadow under a component — the opposite of what
    "follow this signal" needs. It also skips the shadow and AO rays, so
    highlighting is if anything slightly cheaper.
  - *Path tracer*: the net is a real **emitter** — `radiance += throughput *
    glow` at every hit — so it lights the copper and laminate around it and
    shows up in reflections, which the path tracer carries everywhere for
    free. Its metallic is forced to 0 so an emitter does not also mirror the
    sky. `triNet` binds at PT set 0 binding 8, and the highlight rides in
    `PtPush::counts.y`; changing it **resets accumulation** (`setHighlightNet`
    does this), since the converged image is no longer valid.
  - The CPU/Embree tracer does not implement it yet — that path has its own
    material fetch and would need the same triangle-net array.
- Gerber packages carry no nets, so the dock says so instead of showing an
  empty list. Verified the Gerber path is untouched (F.Cu still 293.224 mm²).

## Blind/buried via spans (built 2026-07-19, KiCad only)

A blind via (outer→inner) or buried via (inner→inner) is drilled only through
part of the stack. The importer already excluded them from `board.drills`
(their bore never reaches the outer faces — span judged by LAYERS, never the
`(via buried)` type token, which real boards contradict); now they surface as
**partial-depth bores** instead of vanishing:

- `LayerArt::partialBores` — each bore carries its hole path plus its two end
  copper layers **by name**, so `applyThickness`'s re-stacking moves the bore
  with its layers for free. Resolved to a Z range in `assemble()`: bottom face
  of the lower end foil to top face of the upper one (the drill goes through
  both end foils' copper).
- **Copper**: foils whose centre lies inside the bore get it subtracted — the
  end foils' annular rings become real rings, not solid discs.
- **Substrate**: only the slabs the span crosses are bored; untouched slabs
  reuse the shared shape set (the no-bore fast path — 8-layer cx4 still
  produces 7 identical slabs).
- **Barrels**: one part per distinct span (all named "vias", so one stackup
  toggle), tube over just its Z range, built by the same castellation-filtered
  inset pipeline as full barrels (`makeBarrelPart`).
- **Explode**: a partial barrel is NOT pinned like a full-stack tube — it
  travels WITH the layers it spans. `Part::partialBarrel` flags it; both
  renderers rank it by centre Z **interpolated between the consecutive
  board-layer ranks** (`rankAtZ`, mirrored in renderer.cpp and
  cpu_tracer.cpp — keep in step), fractional so it claims no peel stage.
- Verified on a synthetic 4-layer board (through + blind F–In1 + buried
  In1–In2): OBJ Z-extents exact (0.010–1.590 / 1.040–1.590 / 0.525–1.075),
  per-slab hole counts correct, GPU and CPU exploded renders in identical
  positions, buried barrel visible through a translucent substrate at rest.
- **Gerber packages are excluded**: a Gerber job expresses blind/buried spans
  as separate per-span drill FILES (KiCad exports e.g. `-F_Cu-In1_Cu.drl`),
  which the Gerber importer does not yet map to spans — those drills are
  currently treated as full-stack. Follow-up work.

## Component rendering (KiCad only) — via kicad-cli GLB

Components are the one thing a Gerber package cannot carry, so this is a
KiCad-only feature. `src/app/component_import.cpp` sources them.

**Why kicad-cli, not our own parser.** KiCad 10 ships its 3D model library as
STEP only — no `.wrl` siblings (checked: `C_0603_1608Metric.step` exists,
`.wrl` does not). STEP is B-rep; meshing it needs a full geometry kernel
(OpenCASCADE), which is a huge dependency at odds with "portable, standalone".
The installed kicad-cli *already contains* that kernel, so we let it do the
tessellation: `kicad-cli pcb export glb --no-board-body --subst-models --no-dnp`
emits a components-only GLB (we render our own board body), which we cache and
load with cgltf (single-header, MIT). First open of a board runs the export
(~3 s); every later open reuses the cached GLB and needs neither KiCad nor a
network — the "download once, keep for reference" behaviour, generated locally
from the installed library rather than fetched. `--no-dnp` because the fab does
not place do-not-populate parts (governing principle). Never fatal: no kicad-cli,
or an export/parse failure, drops components and renders the bare board with a
status-bar reason. `PCBVIEW_NO_COMPONENTS` disables; `PCBVIEW_KICAD_CLI`
overrides the discovered exe.

**Coordinate convention — measured, not guessed.** KiCad's GLB is glTF Y-up,
metres, positions carried on node transforms (not baked — 23 nodes carry
translations; `cgltf_node_transform_world` must be composed). Locked from a known
part: C6 is `(at 47 61.5)` in the `.kicad_pcb` and `(0.047, 0.001595, 0.0615)` m
in the GLB. So `world = (gltf_x, -gltf_z, gltf_y) × 1000`. The linear part has
**determinant +1** — a proper rotation — so winding and normals survive; no flip.
Verified across the whole board: every part lands on its pads, top and bottom.

**Explode + thickness integration.** Each glTF node's origin Z (our space,
vs board mid) sets `Part::mountSide` (+1 top / −1 bottom). In the renderer,
component materials are **excluded from the per-layer Z-rank sort** and given
their **own plane one ring beyond the outermost board layer** on their side
(`±(mid+1)`, and `maxRank_` bumped to match) — so components share one stage
(every IC peels together, not 23 separate stages) but that stage is distinct
from the silkscreen's: an IC lifts a full stage clear of the silk it sits on
rather than sharing its plane. A thickness override
shifts top-mounted parts by `(effective − design)` so they stay seated on the
moved surface (`appendComponents`); bottom parts sit on the fixed Z=0 face.
Parts share the name `"Components"` so one stackup toggle drives every
colour/side group at once.

## Roadmap

- **Phase 0** — scaffold, Vulkan device with RT-ready flags, report RT
  capability. **Done**, verified on the RTX 5070 Ti.
- **Phase 1** — **Done.** `BoardModel` IR + KiCad importer, tessellation
  (Clipper2 1.5.4 + earcut v2.2.4), soldermask, and the Vulkan rasterizer.
  592k triangles, validation clean.
- **Phase 1.5** — **Done.** Qt Widgets GUI ("pro CAD" direction): menu bar,
  toolbar, stackup tree with per-layer visibility, properties dock, status bar,
  orbit camera, and an internal-resolution scale (0.25×–4×) that supersamples
  the board while the interface stays native-crisp.
- **Phase 2** — Gerber RS-274X + Excellon importer into the same IR.
- **Phase 3** — soldermask, silkscreen, real materials. **Done.** Components
  **Done** but via kicad-cli GLB, not WRL (KiCad 10 ships STEP-only — see
  "Component rendering" above).
- **Phase 4** — **Done.** Ray-query RT: contact shadows + ambient occlusion from
  the fragment shader, GPU selectable. See "Ray tracing (built)" above.
- **Phase 5** — **Done.** CPU rendering device (lavapipe raster + Embree
  tracing, see "CPU rendering (built)" above), soft sun + laminate
  transmission in both tracers, Effects sliders with readouts, app icon, and
  the Inno Setup installer.
- **Phase 6** — **Done.** Blind/buried via spans, Excellon rout-mode slots,
  measurement tools (snapping, dimension callouts, same-net shortest-route
  panel), globe-spin/roll right-drag, and the screen-space overlay pipeline
  those all draw through.

### Future release ideas (noted 2026-07-19)

- **FIXED post-v1.12 — right-drag is globe-spin + twist (took two tries).**
  v1.12 shipped right-drag as a *mirrored* left-drag. The first fix mapped
  it to yaw + roll — wrong again, because from a top-down view roll and yaw
  are visually the same in-plane spin, so both right-drag axes looked
  identical ("all yaw"). The user's screenshot walkthrough pinned the
  vocabulary: "twist cw/ccw" = the in-plane spin, "rotate left and right
  (spin on axis)" = a **globe spin** — tumbling about the SCREEN-VERTICAL
  axis, the one rotation no drag produced. Final mapping: left-drag =
  yaw + pitch (turntable spin + toward/away tumble); **right-drag
  horizontal = globe spin** (rotate the eye offset about the camera's up
  axis via a Rodrigues helper, then decompose the result back into
  yaw/pitch/roll — a camera-up rotation is not expressible as one turntable
  increment; up itself is invariant, and the residual orientation lands in
  roll), **right-drag vertical = roll** (cw/ccw twist about the view axis).
  User-confirmed 2026-07-19. Supporting infrastructure: `Camera::roll`
  applied in `cameraBasis` AFTER the pole-safe yaw/pitch basis; roll eases
  in view-preset glides, wraps shortest-way, and is cleared by Top/Bottom/
  Iso (canonical views untwist; Fit keeps it). Everything downstream —
  raster view, PT/RT ray camera on both devices, pan — derives from
  `cameraBasis`, so no other plumbing changed. `PCBVIEW_START_ROLL`
  (radians) is the headless hook; raster + PT captures verified at 45°.

- **Measurement tools — BUILT 2026-07-19**, see "Measurement tools (built)"
  below. Both pieces landed: snap-first point-to-point measure and the
  board-dimensions overlay. Notable departures from this note: no Embree
  picking (snapping + a board-plane unproject covered every use case without
  per-frame ray casts), and no QPainter overlay — a native QWindow suffers
  the airspace problem, so the overlay is a small Vulkan triangle pipeline
  in the renderer's UI pass, which also works identically on the CPU device
  and inside PT mode. That pipeline is the one net-trace labels should
  reuse.
- **Net trace / highlighting — BUILT 2026-07-19**, see "Net highlighting"
  below. The sketch held up: searchable net dock, click-to-select on the
  board, dim-everything-else. The one thing the note got wrong was where the
  net id lives — "per-part mesh metadata" would have meant a part per net and
  a draw call per net, so it is per TRIANGLE instead. Still open: the path
  tracers ignore the highlight.
- **Party mode.** Fun mode: multi-coloured spinning lights sweeping and
  highlighting the board, possibly with a disco ball. The path tracer already
  supports arbitrary emissive directions (sun NEE + sky), so animated coloured
  key lights are mostly parameter animation; a disco ball is a mirrored sphere
  mesh + the existing metallic lobe. Needs continuous re-accumulation (or
  raster-mode lights) since PT converges on a *still* scene — raster-mode
  animated lights first, traced version later.
- **Shader plugins (noted 2026-07-19).** User-authored shaders assignable to
  a chosen slice of the stack: hairy soldermask, liquid-metal traces, whatever
  someone dreams up. Feasibility notes: the material table is already bindless
  and indexed per part (RT-readiness rule 3), so "which shader does this part
  use" is a field in that table rather than a re-plumb. The hard parts are
  (a) shipping a compiler — shaders are currently glslc'd at BUILD time into
  the exe, so runtime plugins need either a bundled compiler or pre-compiled
  SPIR-V dropped in a folder; (b) **the two tracers**, since a custom raster
  shader does not exist for the path tracer or Embree, so a plugin either
  declares itself raster-only or supplies a BSDF the tracers can call; and
  (c) sandboxing — arbitrary SPIR-V can hang the GPU, so a plugin needs to
  be opt-in and clearly the user's own risk. Suggested first cut: raster-only
  plugins, SPIR-V or GLSL from `~/.pcbview/shaders/`, assigned per part in
  the stackup tree, with the traced modes falling back to the stock material.
- **Showcase mode (turntable video).** Automated camera choreography — orbit,
  glide, explode/collapse sweeps — driven by the existing animation plumbing
  (view presets + `PCBVIEW_START_*` show the camera is already scriptable),
  recorded and encoded to short video clips. Capture side exists in embryo
  (`PCBVIEW_CAPTURE` grabs the presented frame); a clip recorder is
  frame-sequence capture at a fixed timestep + an encoder. Prefer a
  no-new-DLL route (Windows Media Foundation H.264) over bundling ffmpeg;
  fallback is emitting a PNG sequence the user encodes themselves.
  **Not a live screen grab — this is a record-then-render pipeline (user
  requirement, 2026-07-19).** In PT (and CPU RT) a moving camera never
  converges, so grabbing presented frames would record noise (or the
  fast-movement raster downgrade). Instead: **pass 1** records the
  choreography as a list of per-frame camera + explode states at the output
  timestep; **pass 2** replays it offline, treating every frame as a still —
  park the camera, accumulate to the sample target, denoise (OIDN), *then*
  capture — and encodes the post-processed frames. Frame time is whatever
  convergence takes, decoupled from playback time. The accumulate→denoise→
  resolve machinery is exactly what `PCBVIEW_CAPTURE` + `resolveDisplay`
  already do per still image, so pass 2 is that loop iterated over the
  recorded path. Per-frame cost can drop a lot by warm-starting: consecutive
  frames differ by a tiny camera delta, so fewer samples per frame may
  suffice (or reproject), but correctness first — naive full re-converge per
  frame is acceptable for short clips.

## Test corpus

`C:\Users\drayj\cx4-multicart` has `cx4multicart_v3.kicad_pcb` **and** a Gerber
export of the same board. Both importers must produce geometrically identical
output from it — a far stronger correctness check than eyeballing renders.

## Toolchain (verified on this box 2026-07-16)

- Vulkan SDK `1.4.335.0` (`VULKAN_SDK` set) — provides `glslc`, `dxc`, `slangc`
- Inno Setup 6 (installer compiler) — installed 2026-07-19 via
  `winget install -e --id JRSoftware.InnoSetup`; `ISCC.exe` at
  `%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe`. Build the installer with
  `ISCC.exe installer\pcbview.iss` after the `deploy` target has staged
  `build\Release`; output lands in `installer\Output\`.
- Visual Studio 18 Community — bundles CMake and Ninja at
  `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\`
- KiCad 10.0.4 **is** installed, per-user, at
  `C:\Users\drayj\AppData\Local\Programs\KiCad\10.0` (an earlier note here said
  it was not — that was wrong; a Program Files search missed the per-user
  install). The board importer still needs nothing from it, but component
  rendering shells out to its `bin\kicad-cli.exe` (see "Component rendering").
  Its 3D library is STEP-only — no `.wrl`.

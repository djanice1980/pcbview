# Licensing and third-party notices

## pcbview is GPL-3.0-or-later

Copyright (C) 2026 pcbview contributors.

pcbview is free software: you can redistribute it and/or modify it under the
terms of the **GNU General Public License version 3**, or (at your option) any
later version. See [LICENSE](LICENSE).

### Why GPL, and why version 3 specifically

pcbview renders silkscreen text with KiCad's **Newstroke** stroke font, whose
data is **GPL-2.0-or-later**. Embedding it makes pcbview a derivative work, so
pcbview must be GPL too. That was a deliberate, informed choice (2026-07-17), not
an accident.

**Version 3 is load-bearing, not a preference.** The dependency set is only
mutually compatible at GPL-3:

| Component | Licence | Note |
|---|---|---|
| Newstroke font data | GPL-2.0-**or-later** | the "or later" is what lets us take GPL-3 |
| Qt 6 (Core/Gui/Widgets) | LGPL-3.0 | combines into GPL-3; **incompatible with GPL-2** |
| Vulkan SDK headers/loader | Apache-2.0 | compatible with GPL-3; **incompatible with GPL-2** |
| Clipper2 | BSL-1.0 | permissive |
| earcut.hpp | ISC | permissive |
| glm | MIT | permissive |
| cgltf | MIT | permissive |
| pugixml | MIT | permissive |
| Microsoft compoundfilereader | MIT | permissive (vendored single header) |
| KiCad Altium record layouts | GPL-3.0 | adapted from pcbnew/pcb_io/altium |
| Intel Open Image Denoise | Apache-2.0 | compatible with GPL-3; **incompatible with GPL-2** |
| Intel Embree | Apache-2.0 | compatible with GPL-3; **incompatible with GPL-2** |

Both Qt's LGPL-3 and Apache-2.0 are **incompatible with GPL-2**. Relicensing
pcbview to GPL-2 would therefore be legally impossible while keeping Qt and
Vulkan. Do not "simplify" the licence.

## Qt

**pcbview uses the Qt framework. Qt, and its use in pcbview, are covered by the
GNU Lesser General Public License version 3 (LGPL-3.0).**

Copyright (C) The Qt Company Ltd. and other contributors.

Licences shipped with pcbview:

- [LICENSES/LGPL-3.0.txt](LICENSES/LGPL-3.0.txt) — GNU Lesser General Public License v3
- [LICENSES/GPL-3.0.txt](LICENSES/GPL-3.0.txt) — GNU General Public License v3

Qt source is available from <https://download.qt.io/> and
<https://code.qt.io/cgit/qt/qt5.git/>.

### How pcbview satisfies LGPL-3.0

pcbview is a *Combined Work* under LGPL-3.0 §4 and complies via **option 4(d)(1)**
— a suitable shared library mechanism:

| Obligation | How it is met |
|---|---|
| §4(a) prominent notice that the Library is used and is covered by this Licence | This file, plus the About dialog |
| §4(b) accompany with a copy of the GNU GPL and this Licence | `LICENSES/` ships with every copy |
| §4(c) if copyright notices are displayed during execution, include the Library's and point to the licences | The About dialog names Qt, its copyright, and where to find `LICENSES/` |
| §4(d)(1) suitable shared library mechanism | Qt is **dynamically linked**. The Qt DLLs ship beside the executable and may be replaced by the user with any interface-compatible build |
| §4(e) Installation Information | Not triggered — pcbview is not conveyed as part of a User Product under GPL-3.0 §6 |

§3 (object code incorporating material from Library header files) also applies,
because Qt's headers contain inline functions and templates longer than ten
lines. Its obligations 3(a) and 3(b) are the same notice and licence-copy
requirements already met above.

### Rules this imposes on the build — these are not style preferences

1. **Qt MUST be dynamically linked.** Never statically link it. Static linking
   abandons option 4(d)(1) and forces option 4(d)(0), which would require
   shipping the Corresponding Application Code in relinkable form.
2. **Qt MUST NOT be modified.** Modifying it triggers §2 and obliges us to convey
   the modified Library's source. Use stock Qt binaries.
3. **Nothing may prevent the user replacing the Qt DLLs.** No checksums over
   them, no signature pinning, no loading them from a sealed archive.
4. **Only LGPL-licensed Qt modules may be used.** Some Qt modules are offered
   under GPL-3.0 or a commercial licence *only*. Qt Core, Qt Gui and Qt Widgets
   are LGPL-3.0.
5. **Qt plugins load dynamically** (e.g. the `qwindows` platform plugin). Do not
   link them statically.

## Newstroke font

pcbview embeds glyph data from KiCad's **Newstroke** stroke font, used to render
silkscreen text.

Copyright (C) 2010 Vladimir Uryvaev.
Copyright The KiCad Developers.

Licensed under the **GNU General Public License version 2 or later**; pcbview
exercises the "or later" option and uses it under **GPL-3.0**. The full text is
in [LICENSES/GPL-3.0.txt](LICENSES/GPL-3.0.txt).

Source: `common/newstroke_font.cpp` in the KiCad source tree,
<https://gitlab.com/kicad/code/kicad>.

**Only the Basic Latin glyphs are embedded.** KiCad's file also contains CJK
ideographs (MIT, © 2018 Lingdong Huang) and data derived from Source Han Sans
(SIL OFL 1.1, © 2014-2019 Adobe). pcbview embeds **neither**, so neither licence
applies to it. If CJK glyphs are ever added, both notices must be reproduced
here.

> Note for anyone re-checking this: the SIL OFL text *inside* KiCad's
> `newstroke_font.cpp` covers the Source Han Sans additions only — **not**
> Newstroke itself, which is GPL. Several secondary sources get this wrong.

## Other third-party software

| Component | Licence |
|---|---|
| Clipper2 | Boost Software License 1.0 |
| earcut.hpp | ISC |
| glm | MIT |
| miniz | MIT |
| cgltf | MIT |
| pugixml | MIT |
| Microsoft compoundfilereader | MIT |
| KiCad (Altium importer record layouts) | GPL-3.0 |
| Intel Open Image Denoise | Apache-2.0 |
| Intel Embree | Apache-2.0 |
| Mesa (lavapipe) | MIT |
| Vulkan SDK | Apache-2.0 |

The `OpenImageDenoise*.dll` and `tbb12.dll` files staged beside the executable are
the Intel Open Image Denoise runtime (Apache-2.0), used for neural denoising of the
path-traced image. Apache-2.0 is GPL-3-compatible. The CPU, CUDA (NVIDIA) and HIP
(AMD) device runtimes are bundled; OIDN selects the fastest available GPU at
runtime, falling back to the CPU. The SYCL device is not bundled.

The `embree4.dll`, `tbb12.dll` and `tbbmalloc.dll` files staged beside the
executable are **Intel Embree** (Apache-2.0) and its oneTBB runtime (also
Apache-2.0), used for CPU ray tracing: when the CPU rendering device is
selected, both the ray-traced preview and the path tracer run their ray
traversal through Embree instead of the GPU's ray-query hardware. Apache-2.0 is
GPL-3-compatible.

The `vulkan_lvp.dll` file (with `lvp_icd.x86_64.json`) staged beside the executable
is Mesa's **lavapipe**, a software (CPU) Vulkan driver, so the board can be rendered
without a capable GPU. It is a prebuilt package from the
[mesa-dist-win](https://github.com/pal1000/mesa-dist-win) project; Mesa is
MIT-licensed and GPL-3-compatible. pcbview registers it additively at startup, so
it appears as an optional "llvmpipe" CPU device alongside any hardware GPUs and is
never chosen automatically.

All permissive and GPL-3-compatible.

# RT64 Fast renderer

This optional renderer retains RT64's microcode identification and GBI decoders,
performs vertex/TMEM processing on the CPU, and submits geometry through a small
draw interface. It targets vitaGL on PS Vita; the GLES2 build exercises the same
draw and shader code in host tests. The default desktop renderer is unchanged.

```sh
cmake -S . -B build-fast -DRT64_FAST=ON -DRT64_FAST_TESTS=ON
cmake --build build-fast
ctest --test-dir build-fast --no-tests=error --output-on-failure
```

`RT64_FAST_VITAGL=ON` selects the Vita backend; `RT64_FAST_GLES2=ON` selects host
GLES2. They are mutually exclusive. The Vita build needs vitaGL, vitaShaRK and
the VitaSDK toolchain. Build vitaGL with `NO_SPLASHSCREEN=1` for Vita3K and
`STORE_DEPTH_STENCIL=1` to preserve depth across GPU scenes.

Build vitaGL with `HAVE_SHADER_CACHE=1` to persist compiled shaders between
launches. Before creating the Vita sink, the application can call
`vglSetShaderCachePath()` with its own writable data directory. Use a cache
namespace tied to the vitaGL/vitaShaRK versions, compiler options and semantic
binding mode; this backend uses `VGL_MODE_SHADER_PAIR`. vitaGL hashes shader
sources and creates its vertex/fragment cache subdirectories. An absent entry
is compiled normally. This avoids repeat compilation stalls, but new shader
combinations still compile on first use; it does not improve steady-state draw
throughput or replace framebuffer synchronization.

Combiner programs retain the last submitted float uniform values and skip
unchanged values and absent uniforms. The cache is per program and preserves
bit patterns, including signed zero. On vitaGL, draw submission queries the
actual bound program before rebinding it: a redundant `glUseProgram` otherwise
marks every constant dirty again. Internal blits and external GL program
changes are therefore observed without relying on a stale binding cache.

The interface supports triangles/rectangles, generated color combiners, batched
draws, VI visibility/gamma/field addressing, and explicit color-image readback.
Readback returns top-to-bottom bytes in N64 RGBA16/RGBA32 order; integration must
synchronize CPU framebuffer consumers and convert the guest's memory byte order.
Host tests cover readback packing and ranges, but Vita3K's macOS Vulkan surface
readback is a known validation limitation.

VI presentation can also create a view from CPU-written RGBA16/32 RAM without a
prior RDP draw. CPU edits reach subsequent scanouts, and those views do not claim
GPU ownership. Reads of fully CPU-owned resident ranges return the original RAM
bytes rather than round-tripping through the GPU. CPU-only views can be retired
under cache pressure. This fallback uses the shared VI size estimate when its
width matches the RAM stride; interlaced stride reinterpretation remains open.

For an opt-in delayed-readback build, compile vitaGL with
`READBACKS_SPEEDHACK=1` and configure RT64 with
`-DRT64_FAST_READBACKS_SPEEDHACK=ON`. The matching RT64 option removes its explicit
`glFinish` immediately before `glReadPixels`, so it does not defeat vitaGL's
speedhack. Normal builds keep the wait. This mode can return earlier frame data;
its effect on game performance and readback-dependent effects needs device tests.

Framebuffer texture loads retain TMEM byte provenance and immutable GPU snapshots.
Compatible RGBA16/32 rectangles sample the captured image directly, including
subregions, split load/render tiles and RGBA32 TMEM banks. Later draws cannot
change an already loaded image. Mixed layouts and format reinterpretations fall
back to materializing the snapshot through CPU readback.

The GL sink observes changes against a shadow of word-swapped RDRAM. A GPU merge
preserves untouched framebuffer bytes while replacing changed bytes, including
partial RGBA16 pixels and RGBA32 alpha. This avoids CPU readback for that merge.
For stores that leave RAM unchanged, integrations can install a watched-range
callback with `setMemoryWriteTracking` and deliver `FastMemoryWrite` byte masks
through `notifyMemoryWrites`. The sink updates every resident target intersecting
those records, including same-value stores, without modifying earlier snapshots.
Flush earlier work before delivering writes at graphics task/readback/presentation
boundaries. RAM comparison remains a fallback for writes that were not reported.
Overlapping RGBA16/32 color images retain separate address, dimension and format
views. A GPU copy merges overlapping N64 bytes in draw order, including changed
row widths and partial pixels, without a CPU readback. View retirement preserves
a complete backing image for later reads and snapshots; the cache consolidates
containing views before dropping their smaller representations. Complete CPU
overwrites release the corresponding GPU-owned byte ranges so ordinary texture
loads can return to RAM. A snapshot/read still requires one retained view to
contain the requested range.

Host regressions cover odd byte addresses, addresses above 16 MiB, format and
stride changes, CPU writes, immutable snapshots and cache pressure. The native
Vita3K/Vulkan diagnostic also preserves overlapping red/blue/green images through
an RGBA16-to-RGBA32-to-RGBA16 round trip. Byte selection uses arithmetic masks:
chained early returns selected the wrong third byte on the tested Vita shader
compilation path, even though the same GLSL passed the host checks.

Color storage is independent of depth surfaces. Draws with the same depth address
and dimensions share one persistent depth FBO, switching its color attachment as
needed. This also works with vitaGL, whose renderbuffer handles do not own shared
depth storage. Changing the depth image preserves color; switching back restores
the previous depth values. Far-depth clears use clipped rectangle draws into a
private color attachment, avoiding the tested vitaGL/Vita3K depth-only clear issue.
Different-size depth interpretations and color/depth memory aliasing remain open.

This is a reduced renderer under development, not full RT64 compatibility.
General framebuffer coherence, remaining VI stride/scaling, extended GBI and unsupported
microcodes need further work. Existing DK64 boot/gameplay evidence does not prove
support for other games or performance on physical Vita hardware.

See the [DK64 Vita integration and validation record](https://github.com/devnoname120/Donkey-Kong-64-Recompiled-Vita/blob/dev/vita/docs/VITA.md).

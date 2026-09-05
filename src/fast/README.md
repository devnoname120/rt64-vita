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

The interface supports triangles/rectangles, generated color combiners, batched
draws, VI visibility/gamma/field addressing, and explicit color-image readback.
Readback returns top-to-bottom bytes in N64 RGBA16/RGBA32 order; integration must
synchronize CPU framebuffer consumers and convert the guest's memory byte order.
Host tests cover readback packing and ranges, but Vita3K's macOS Vulkan surface
readback is a known validation limitation.

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
Overlapping GPU framebuffer aliases still require further work.

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

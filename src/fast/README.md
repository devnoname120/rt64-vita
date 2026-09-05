# RT64 Fast renderer

This optional renderer retains RT64's microcode identification and GBI decoders,
performs vertex/TMEM processing on the CPU, and submits geometry through a small
draw interface. It targets vitaGL on PS Vita; the GLES2 build exercises the same
draw and shader code in host tests. The default desktop renderer is unchanged.

```sh
cmake -S . -B build-fast -DRT64_FAST=ON -DRT64_FAST_TESTS=ON
cmake --build build-fast
ctest --test-dir build-fast --output-on-failure
```

`RT64_FAST_VITAGL=ON` selects the Vita backend; `RT64_FAST_GLES2=ON` selects host
GLES2. They are mutually exclusive. The Vita build needs vitaGL, vitaShaRK and
the VitaSDK toolchain. Use vitaGL's `NO_SPLASHSCREEN=1` option for Vita3K.

The interface supports triangles/rectangles, generated color combiners, batched
draws, VI visibility/gamma/field addressing, and explicit color-image readback.
Readback returns top-to-bottom bytes in N64 RGBA16/RGBA32 order; integration must
synchronize CPU framebuffer consumers and convert the guest's memory byte order.
Host tests cover readback packing and ranges, but Vita3K's macOS Vulkan surface
readback is a known validation limitation.

This is a reduced renderer under development, not full RT64 compatibility.
Framebuffer feedback, remaining VI stride/scaling, extended GBI and unsupported
microcodes need further work. Existing DK64 boot/gameplay evidence does not prove
support for other games or performance on physical Vita hardware.

See the [DK64 Vita integration and validation record](https://github.com/devnoname120/Donkey-Kong-64-Recompiled-Vita/blob/dev/vita/docs/VITA.md).

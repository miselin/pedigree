# New libui port boundary

The new window-manager stack is being brought to Pedigree in layers. The
hosted SDL implementation remains the fast feedback loop; the Pedigree build
provides the same public boundaries with native implementations.

## Current layers

- `libui-core` contains platform-neutral value types and input records.
- `libui-platform` maps the boot-selected framebuffer and turns native input
  callbacks into a pollable event stream. It uses the framebuffer's real GOP
  stride rather than assuming tightly packed rows.
- `libui-buffer` maps each client buffer through `SharedIpcMessage`. A resize
  is represented by new buffers and a new generation; mapped buffers are never
  resized in place.
- `libui-transport` provides the pathname Unix stream connection used by the
  custom RPC layer. Listener cleanup is part of the listener lifetime.
- `libui-protocol` contains the protobuf schema and generated-code build rule.
  It is enabled automatically when the protobuf and Abseil PUP closure is
  installed in the target staging root.

The `winman-gop-smoke` application is deliberately small. It validates that a
UEFI/GOP-selected mode can be opened, painted with Cairo using its actual
stride, flushed, and fed native input without involving the compositor or
client protocol yet.

## Building

The protocol layer is enabled automatically when the default target PUP prefix
contains the protobuf package. Without that package, the legacy userspace
applications can still be built independently:

```sh
cmake -S . -B build
cmake --build build --target app-winman-gop-smoke libui-buffer libui-transport
```

The repository's ignored `images/local/usr` staging tree is the default target
prefix. Install the matching protobuf and Abseil PUPs there, or point
`PEDIGREE_PROTOBUF_PREFIX` at another prefix containing `include`, `lib`, and
the CMake package files. Then enable the protocol layer with:

```sh
cmake -S . -B build \
  -DPEDIGREE_BUILD_LIBUI_PROTOCOL=ON \
  -DPEDIGREE_PROTOBUF_PREFIX=/path/to/pup-prefix \
  -DPEDIGREE_PROTOC_EXECUTABLE=/path/to/host/protoc
```

Pass `-DPEDIGREE_BUILD_LIBUI_PROTOCOL=OFF` when the staged package is present
but the protocol is not part of a particular build.

`protoc` runs during the host-side build, while the generated sources link
against the target PUP runtime. The current default explicitly permits the
nearby host 35.1/target 35.0 pairing by suppressing protobuf's generated-code
version guard. This is a deliberate bridge for this protocol's limited use of
the C++ API, not a general protobuf compatibility guarantee; use matching
versions when they are available by setting
`-DPEDIGREE_PROTOBUF_ALLOW_GENERATOR_MISMATCH=OFF`.

The compositor and client/widget libraries remain in the separate
`pedigree-winman` checkout so their protocol, widgets, and client API move as a
single versioned unit. The in-tree `gears` and `uitest` sources are compiled by
that external target build. Its terminal client supersedes the old in-tree
`TUI` application. The deprecated `winman` and `TUI` targets are available only
with `-DPEDIGREE_BUILD_LEGACY_WINMAN=ON`.

## GOP smoke boot

To boot the small framebuffer smoke app without starting `ttyterm`, run:

```sh
scripts/run-winman-gop-smoke-qemu.sh
```

The helper temporarily adds a marker to the ignored `images/local` overlay,
rebuilds the UEFI image, removes the marker, and starts QEMU with serial output
enabled. The source overlay remains unchanged, but the generated
`build/pedigree-uefi.img` remains the smoke profile after QEMU exits. To restore
the ordinary image, remove the two generated image files and rebuild:

```sh
rm -f build/pedigree-uefi-root.img build/pedigree-uefi.img
cmake --build build --target uefi-image -j2
```

Stop the smoke app with `Ctrl-C` in the host terminal.

## Out-of-tree window-manager build

The complete compositor/client/widget implementation lives in the separate
`pedigree-winman` checkout. A normal amd64 build discovers an adjacent checkout
and builds it after the Pedigree target libraries, installing the compositor,
clients, font, and init script into the ignored `images/local` overlay. Set
`PEDIGREE_WINMAN_SOURCE_DIR` when the checkout lives elsewhere, or disable the
integration with `-DPEDIGREE_BUILD_EXTERNAL_WINMAN=OFF`. Existing build trees
retain an explicitly selected ON/OFF value in their CMake cache.

If the checkout is absent, the external window manager and windowed clients are
simply omitted. The direct-framebuffer `gfxcon` fallback and the new-platform
`winman-gop-smoke` diagnostic remain in the Pedigree build.

The external build can also be run directly:

```sh
cd /Users/miselin/src/pedigree-winman
PEDIGREE_ROOT=/Users/miselin/src/pedigree tools/build-pedigree.sh
```

When invoked directly, the target build and generated sources remain in the
window-manager checkout. The integrated target uses a path-specific directory
under the Pedigree build tree instead. In both cases Pedigree provides the SDK
artifacts and snapshots the staged files into the UEFI image. Stop any QEMU
instance using the image before rebuilding it.

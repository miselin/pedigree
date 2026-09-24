# Pedigree window-manager build

The compositor, client protocol, widgets, and terminal application live in the
separate `pedigree-winman` checkout. Pedigree supplies the framebuffer and input
interfaces through `libfb`, `libpedigree`, and `libpedigree-c`. The in-tree
`gears` and `uitest` sources are compiled by the external build with its matching
client libraries.

A normal amd64 build discovers an adjacent checkout and builds it after the
Pedigree target libraries, installing the compositor, clients, font, and init
script into the ignored `images/local` overlay. Set `PEDIGREE_WINMAN_SOURCE_DIR`
when the checkout lives elsewhere, or disable integration with
`-DPEDIGREE_BUILD_EXTERNAL_WINMAN=OFF`. Existing build trees retain an explicitly
selected ON/OFF value in their CMake cache.

If the checkout is absent, the external window manager and windowed clients are
omitted. `ttyterm` provides the text and serial console.

The external build can also be run directly:

```sh
cd ../pedigree-winman
PEDIGREE_ROOT=../pedigree tools/build-pedigree.sh
```

When invoked directly, the target build and generated sources remain in the
window-manager checkout. The integrated target uses a path-specific directory
under the Pedigree build tree. Pedigree snapshots the staged files into the UEFI
image. Stop any QEMU instance using the image before rebuilding it.

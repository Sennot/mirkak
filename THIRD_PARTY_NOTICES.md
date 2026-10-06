# Third-party notices

## Spout2

- Source: https://github.com/leadedge/Spout2
- SDK version: `2.007.017`
- License: BSD 2-Clause; see [licenses/Spout2-BSD-2-Clause.txt](licenses/Spout2-BSD-2-Clause.txt).
- Use: the SpoutGL implementation is vendored under `third_party/spout` and statically linked into the mod DLL.

## GDH (reference only)

- Source: https://gitlab.com/tobyadd/GDH (`src/hacks/level/layout_mode.cpp`)
- Use: the behaviour of Layout Mode (which objects count as decoration, the default background,
  ground and line colors) follows this reference. No GDH code is included; the decoration object
  ID list is stored in this mod as ranges in `src/layout/object_roles.cpp`.

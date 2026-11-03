# Third-party notices

## Spout2

- Source: https://github.com/leadedge/Spout2
- SDK version: `2.007.017`
- License: BSD 2-Clause; see [licenses/Spout2-BSD-2-Clause.txt](licenses/Spout2-BSD-2-Clause.txt).
- Use: the SpoutGL implementation is vendored under `third_party/spout` and statically linked into the mod DLL.

## Silicate

- Source: https://git.silicate.dev/silicate/silicate
- Snapshot used: commit `f183dbc520cc01f97e3d1cb6c7e77e70d3b30aa6` (2026-08-04), through the Spout2 Clean Feed mod.
- License: GNU General Public License v3.0; the full license is the repository root [LICENSE](LICENSE).
  Because this code is included, Spout2 Layout Feed is distributed under the GPL-3.0 as well.
- Use: player/object physics, collision prediction, gravity behavior, checkpoint state and the trajectory
  predictor (`src/physics`, `src/checkpoint`, `src/trajectory`). Drawing was moved to this mod's GPU shape shader.

## Eclipse Menu

- License: Eclipse Public License 2.0; see [licenses/Eclipse-EPL-2.0.md](licenses/Eclipse-EPL-2.0.md).
- Use: hitbox category selection and visible-section traversal informed `src/overlay/hitboxes.cpp` (through the
  Spout2 Clean Feed mod).

## Spout2 Clean Feed (reference)

- The user's earlier mod, supplied in `reference/` (v1.7.0). Hitboxes, trajectory prediction, Smart Contrast,
  their settings and keybinds were ported from it. The Spout2 sender, the swapBuffers hook ordering
  around Mega Hack, the OpenGL driver present hook (`src/present_hook.cpp`) and the visible-section traversal (itself informed by Eclipse Menu, EPL-2.0) come
  from it.

## GDH (reference only)

- Source: https://gitlab.com/tobyadd/GDH (`src/hacks/level/layout_mode.cpp`)
- Use: the behaviour of Layout Mode (which objects count as decoration, the default background,
  ground and line colors) follows this reference. No GDH code is included; the decoration object
  ID list is stored in this mod as ranges in `src/layout/object_roles.cpp`.

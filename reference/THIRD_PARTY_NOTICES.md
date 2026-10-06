# Third-party notices

## Silicate

- Source: https://git.silicate.dev/silicate/silicate
- Snapshot used: commit `f183dbc520cc01f97e3d1cb6c7e77e70d3b30aa6` (2026-08-04)
- License: GNU General Public License v3.0; the full license is the repository root [LICENSE](LICENSE).
- Use: player/object physics, collision prediction, gravity behavior, checkpoint state and the trajectory predictor were adapted for a standalone Geode mod. Bot, replay, UI and Silicate renderer coupling were removed.

## Spout2

- Source: https://github.com/leadedge/Spout2
- SDK version: `2.007.017`
- License: BSD 2-Clause; see [licenses/Spout2-BSD-2-Clause.txt](licenses/Spout2-BSD-2-Clause.txt).
- Use: the required SpoutGL implementation is vendored under `third_party/spout` and statically linked into the mod DLL.

## Eclipse Menu

- Source reference: the `EclipseMenu-1.9.4` checkout supplied beside this project.
- License: Eclipse Public License 2.0; see [licenses/Eclipse-EPL-2.0.md](licenses/Eclipse-EPL-2.0.md).
- Use: hitbox category selection and visible-section traversal informed the new implementation in `src/hitboxes.cpp`.

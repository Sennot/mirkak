# Changelog

## v2.3.1
- Fixed: the isolation of level B was not active in v2.3.0. Every direct call was refused because
  TulipHook calls GD's code through a small wrapper, not the relocated prologue the check expected,
  so Mega Hack still saw level B (thousands of noclip deaths while touching a spike: level B's
  frames reset its per-frame death flag). The wrapper is now accepted; the handler and module checks
  stay.
- Fixed: icons of other game modes and labels staying on screen in level B. Only level A's real
  players are drawn on top of level B (not the trajectory's simulation players), and only parts GD
  itself would draw (every parent visible). Player-like nodes and labels that GD or mods put into
  level B are hidden.

## v2.3.0
- Level B is invisible to other mods. Its simulation no longer adds deaths to noclip accuracy, doubles
  CPS counters, shows Mega Hack's noclip tint (the color change on a section), or reaches bots and
  death trackers. While level B runs, about 80 GD functions that Mega Hack, xdBot, CPS counters and
  death trackers hook go straight to GD's original code; deaths, inputs, jumps, statistics, pausing,
  audio and the end of the level never happen for level B at all.
- Much closer full simulation:
  - before every update, level B's players take the exact state of level A's players (position,
    velocity, ground state, speed, gravity, mode, size, held buttons, timers);
  - every press and release reaches level B's players at the moment GD applies it to level A, through
    GD's own pushButton/releaseButton (orbs, pads and touch triggers of level B react to them);
  - level B's camera follows exactly where level A's player is;
  - restart, practice checkpoints (including auto checkpoints), removing checkpoints and leaving
    practice are repeated on level B; level B's own checkpoint objects stay passive.
- Level B is created with GD's original PlayLayer::init (no other mod's init hook sees it).
- Trajectory: predicted deaths are consumed before Mega Hack's noclip even after it re-enables its
  hook (fixed priority instead of an order relative to other mods).

## v2.2.0
- Level Overlay: you play level A (the level you open) while your screen shows level B in its place,
  fully drawn with its objects, colors, background and shaders. OBS still receives level A.
  Level B is picked by the ID of a downloaded online level or by the name of a created level.
- Level B runs as a second, never-shown PlayLayer next to level A: every update its players take
  level A's position, speed, game mode and held buttons, so its triggers, moving groups, colors and
  camera play along. Resets, practice checkpoints and the practice toggle are mirrored.
- Level B is silent and never dies or finishes; its attempts, jumps and statistics are not counted.
- Your player, hitboxes, trajectory, progress bar, pause menu and popups of level A are drawn on top.
- Toggle with F7. While hidden, level B keeps running so it is in sync when shown again.
  Turning the feature off in a level stops the simulation at once.
- "Static objects only" option: level B is not simulated, it takes level A's camera and only shows
  its objects where they were placed. Much cheaper.
- Performance: level B replaces the layout pass on the monitor (no third render); its simulation and
  draw time appear in the debug statistics as level-b-sim and level-b.

## v2.1.0
- Hitboxes and trajectory prediction from Spout2 Clean Feed (v1.7.0), on your screen only. OBS still
  receives the clean level. Works with Layout Mode on (drawn inside the layout pass, following the
  camera) and off, in levels and in the editor.
- New GPU shape shader for both: per shape the CPU writes only its description, fill, border and
  antialiasing are computed per pixel; no CCDrawNode outline tessellation. Object hitboxes now follow
  the view up to 240 times per second (the clean-feed mod capped them at 60), with the same adaptive
  back-off on dense sections. Circles are exact instead of 12-sided polygons.
- Trajectory physics, checkpoints, track merging and the prediction cache are unchanged.
- All clean-feed overlay settings: toggles and keybinds (F3 hitboxes, F4 trajectory), trajectory
  length, TPS, width, high-performance mode and colors, hitbox width, fill opacity and colors, and
  Smart Contrast.
- The mod is now GPL-3.0 because it contains the Silicate-derived prediction code.

## v2.0.5
- Saws and slopes with hitboxes hidden by alpha: their DontDraw sprites are not in the batch atlas
  (GD draws them itself), so the layout shader could not make them opaque. They now count as
  drawn only when fully opaque and visible; otherwise they are rebuilt from GD's geometry.
- Dumps show batch, atlas index, descendant membership and quad position of every sprite.

## v2.0.4
- Fixed invisible saws with hitboxes (and other objects GD positions itself, such as slopes):
  sprites flagged DontDraw are now revealed from the geometry GD wrote for them, with the correct
  rotation, instead of being skipped. Whether the game draws a sprite is judged from the quad in
  the batch atlas, i.e. what the GPU actually receives.
- Object dumps also show the atlas quad state.

## v2.0.3
- New option "Show animated decoration" (on by default): decoration that GD rotates or animates,
  such as decorative saws and gears, stays on screen in Layout Mode. Object dumps showed that the
  missing saws were decoration objects without hitboxes; no object with a hitbox was hidden.

## v2.0.2
- Fewer OpenGL state queries when compositing mod menus (one save/restore for both outputs);
  each query stalls on NVIDIA's threaded driver.
- Debug: "Dump objects on screen" keybind (F8) logs how every kind of object on screen is
  classified and drawn, and the menu compositing cost appears in the statistics.

## v2.0.1
- Fixed saws and other animated or rotating objects (GD special layers) vanishing in Layout Mode:
  when a special-layer container draws nothing in the screen-only pass, its children are drawn
  through the layout roles instead.
- Debug statistics for special-layer sprites in the normal frame and in the layout pass.

## v2.0.0
- Invisible levels: objects are revealed whenever the game would not actually draw them (hidden
  parent, collapsed quad, Hide option, alpha 0), including composite objects and detail sprites
  that sit beside their object in a batch.
- Detail and glow sprites are registered when GD activates an object, so fills of blocks keep
  the layout colors and forced opacity.
- New option: hide pulse circles of orbs, pads, portals and pickups on screen.
- Performance: own open-addressing role cache, role masks rebuilt incrementally (only new
  sprites are classified), invisible-object candidates refreshed at most 120 times per second
  with per-frame checks only for gameplay objects.

## v1.6.1
- Fixed spike slopes drawn rotated by 90 degrees in Layout Mode.

## v1.6.0
- Reveal objects at opacity 0, text objects and objects moved while invisible.

## v1.5.0
- Always show the player icon on screen in Layout Mode.

## v1.4.0
- Mega Hack and Steam overlay in OBS during levels.

## v1.3.0
- Fixed vanishing portals and flickering objects.

## v1.2.0
- Steam overlay capture option, OpenGL state fixes for Mega Hack.

## v1.1.0
- Mod menus in OBS, invisible objects revealed, stale role cache fix.

## v1.0.0
- First release: Layout Mode on screen, normal level in OBS through Spout2.

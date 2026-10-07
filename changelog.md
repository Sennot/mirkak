# Changelog

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

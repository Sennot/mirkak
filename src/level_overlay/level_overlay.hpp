#pragma once

#include <Geode/Geode.hpp>

// Level Overlay: the player plays level A (the level that was opened) while
// the monitor shows level B in its place. Level B is a second PlayLayer that
// never enters the scene. It runs GD's own code only (isolation.cpp keeps
// other mods out of it) and is driven by level A:
//   - before every update of level A, level B's players take the exact state
//     of level A's players (position, velocity, mode, held buttons);
//   - every press and release of level A reaches level B's players at once,
//     through GD's original pushButton/releaseButton;
//   - after the update of level A, level B updates with the same time step,
//     and its camera follows level A's player;
//   - restarts, checkpoints and practice mode of level A are repeated on B.
// It is drawn by hand instead of the layout pass; OBS keeps receiving level A.
namespace layoutfeed::level_overlay {
    // GD code of level B is running (creation, update, reset, checkpoints,
    // drawing). GameManager reports level B as the current level meanwhile.
    bool inLevelB();

    bool isLevelA(GJBaseGameLayer const* layer);
    bool isLevelB(GJBaseGameLayer const* layer);
    bool owns(GJBaseGameLayer const* layer);
    bool owns(PlayerObject const* player);

    // The calls below are made from hooks that run right before GD's own
    // code (priority INT32_MAX - 1). Calls into level B from there go straight
    // to GD's original function, past every other hook (TulipHook continues a
    // nested call of the same function at the next detour).

    // Around level A's PlayLayer::init.
    void beforeLevelInit();
    void afterLevelInit(PlayLayer* levelA, bool success);

    // Around every update of level A.
    PlayLayer* drivenBy(GJBaseGameLayer* layer);
    void beforeUpdate(PlayLayer* levelA);
    void afterUpdate(PlayLayer* levelA, float dt);

    // Level A events repeated on level B.
    void afterStart(PlayLayer* levelA);
    void afterReset(PlayLayer* levelA);
    void afterCreateCheckpoint(PlayLayer* levelA, CheckpointObject* checkpoint);
    void afterStoreCheckpoint(PlayLayer* levelA, CheckpointObject* checkpoint);
    void afterRemoveCheckpoint(PlayLayer* levelA, bool first);
    void afterRemoveAllCheckpoints(PlayLayer* levelA);
    void afterPracticeToggle(PlayLayer* levelA);
    void mirrorButton(PlayerObject* player, PlayerButton button, bool down);

    // Inside level B's updateCamera: its players move to where level A's
    // players are, so the camera follows the real player exactly.
    void placeLevelBPlayers(GJBaseGameLayer* layer);

    void release(char const* reason);

    // Level B exists, the overlay is enabled and shown, and level A is the
    // running level.
    bool shouldRender();

    // Draws level B onto the current framebuffer, then level A's players,
    // hitboxes, trajectory, interface and the layers above the level (pause
    // menu, end screen, popups). Replaces the layout pass for that frame.
    void render();

    // Logs every player node of level A and level B on the next drawn frame
    // (debug dump keybind).
    void requestDump();
}

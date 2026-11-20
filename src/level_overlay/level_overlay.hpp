#pragma once

#include <Geode/Geode.hpp>

// Level Overlay: the player plays level A (the level that was opened) while
// the monitor shows level B in its place. Level B is a second PlayLayer that
// never enters the scene: it is created before level A, ticked right after
// every update of level A with its players following level A's players, and
// drawn by hand instead of the layout pass. OBS keeps receiving level A.
namespace layoutfeed::level_overlay {
    // Level B's PlayLayer::init is running.
    bool constructing();
    // Code of level B is running (init, simulation, reset, draw). GameManager
    // reports level B as the current PlayLayer meanwhile.
    bool inLevelB();

    bool isLevelA(GJBaseGameLayer const* layer);
    bool isLevelB(GJBaseGameLayer const* layer);

    // Around level A's PlayLayer::init. Level B is created first, so mods that
    // remember the PlayLayer from their own init hook end up with level A.
    void beforeLevelInit();
    void afterLevelInit(PlayLayer* levelA, bool success);

    // Around every update of level A.
    void beforeUpdate(PlayLayer* levelA);
    void afterUpdate(PlayLayer* levelA, float dt);

    // Level A events mirrored to level B.
    void onReset(PlayLayer* levelA);
    void onCheckpointStored(PlayLayer* levelA);
    void onCheckpointRemoved(PlayLayer* levelA, bool first);
    void onPracticeToggled(PlayLayer* levelA);

    void release(char const* reason);

    // Level B exists, the overlay is enabled and shown, and level A is the
    // running level.
    bool shouldRender();

    // Draws level B onto the current framebuffer, then level A's players,
    // hitboxes, trajectory, interface and the layers above the level (pause
    // menu, end screen, popups). Replaces the layout pass for that frame.
    void render();
}

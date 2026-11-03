#pragma once

#include <Geode/Geode.hpp>

namespace layoutfeed::player_overlay {
    // Root of the screen-only overlays (hitboxes, trajectory). It is a child
    // of the object layer, so the camera, zoom and rotation apply to it, and
    // it draws only:
    //   - inside the layout pass (Layout Mode on), as part of the object layer;
    //   - through drawForPlayer() after the Spout2 capture otherwise.
    // OBS therefore never receives it.
    void attach(GJBaseGameLayer* layer);
    void detach(GJBaseGameLayer* layer);

    cocos2d::CCNode* root();
    GJBaseGameLayer* layer();

    // Draws the overlay onto the current framebuffer with the object-layer
    // transform. Used when no layout pass ran this frame.
    void drawForPlayer();
}

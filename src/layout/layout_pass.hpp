#pragma once

#include <Geode/Geode.hpp>

namespace layoutfeed::pass {
    enum class BatchKind {
        Other,  // UI, ground, middleground: drawn normally
        Object, // level object batch: drawn by the layout renderer
        Glow,   // level glow batch
    };

    // True only while the screen-only layout re-render is visiting the scene.
    bool active();
    BatchKind classify(cocos2d::CCSpriteBatchNode* batch);

    // A PlayLayer is running and Layout Mode is enabled.
    bool shouldRender();

    // Redraws the running scene onto the back buffer in Layout Mode. Called
    // after the normal frame has been sent to Spout2, before the swap.
    void render();

    void onLevelEnter(PlayLayer* layer);
    void onLevelExit(PlayLayer* layer);
}

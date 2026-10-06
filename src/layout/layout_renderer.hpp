#pragma once

#include <Geode/Geode.hpp>

#include <vector>

namespace layoutfeed::renderer {
    // Compiles the layout shader on first use. Returns false when the driver
    // cannot run it; the layout pass then still removes background, ground
    // colors and shaders, and object batches are drawn unchanged.
    bool ensureReady();
    bool ready();

    // Uploads per-pass uniforms (colors, opacity mode).
    void beginPass();

    // Draws one level object batch with per-quad roles evaluated on the GPU.
    // Returns false when the caller must fall back to the original draw.
    bool drawObjectBatch(cocos2d::CCSpriteBatchNode* batch);

    // Draws revealed (otherwise invisible) object quads as triangles with
    // final vertex colors, in the current modelview space.
    void drawRevealed(cocos2d::CCTexture2D* texture, std::vector<cocos2d::ccV3F_C4B_T2F> const& vertices);

    // Releases the per-batch role textures (level exit).
    void releaseCaches(char const* reason);
}

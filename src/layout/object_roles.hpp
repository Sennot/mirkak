#pragma once

#include <Geode/Geode.hpp>

#include <cstdint>

namespace layoutfeed::roles {
    // How a quad is drawn in the screen-only layout pass. The values are read
    // by the layout vertex shader, so keep them in sync with layout_renderer.
    enum class Role : std::uint8_t {
        Keep = 0,   // original colors (player, unknown sprites, portals' own art)
        Hide = 1,   // not drawn (decoration, no-touch objects)
        Main = 2,   // drawn with the layout object color
        Detail = 3, // drawn with the layout detail color
    };

    // Starts collecting sprites of a new PlayLayer and forgets the previous one.
    void reset(char const* reason);
    // Stops registering new objects; known roles remain for the quit transition.
    void stopCollecting();

    void registerObject(GameObject* object);
    void registerGlow(GameObject* object);

    // Resolves the role of a sprite drawn in the layout pass. Unknown sprites
    // are classified once (RTTI walk to their GameObject) and then cached.
    Role resolve(cocos2d::CCNode* sprite);

    // Changes whenever a resolved role can differ from before: new sprites,
    // or settings that affect the role mapping. Flag caches compare it.
    std::uint32_t generation();

    void logSummary();
}

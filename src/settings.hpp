#pragma once

#include <Geode/Geode.hpp>

#include <cstdint>
#include <string>

namespace layoutfeed::settings {
    // Spout2
    bool enabled();
    std::string const& senderName();
    bool captureCursor();
    bool captureMenus();
    bool captureSystemOverlays();

    // Layout Mode
    bool layoutEnabled();
    bool hideDecoration();
    bool recolorObjects();
    bool forceOpacity();
    bool normalBlending();
    bool hideGlow();
    bool hideParticles();
    bool hideMiddleground();
    bool disableShaders();
    bool alwaysShowPlayer();
    cocos2d::ccColor3B objectColor();
    cocos2d::ccColor3B detailColor();
    cocos2d::ccColor3B backgroundColor();
    cocos2d::ccColor3B groundColor();
    cocos2d::ccColor3B lineColor();

    // Debug
    bool debugLogging();
    int statsInterval();
    bool gpuTimers();

    // Incremented whenever any setting changes. Caches derived from settings
    // compare against it instead of re-reading Geode setting objects per frame.
    std::uint32_t generation();

    void logSummary();
}

#pragma once

#include <Geode/Geode.hpp>

#include <cstdint>
#include <string>

namespace layoutfeed::settings {
    // Overlay colors, same keys and order as the clean-feed mod.
    enum class Color {
        Solid,
        Hazard,
        Interactable,
        Player,
        PlayerInner,
        PlayerRotated,
        TrajectoryHold,
        TrajectoryRelease,
        Count,
    };

    // Spout2
    bool enabled();
    std::string const& senderName();
    bool captureCursor();
    bool captureMenus();
    bool captureSystemOverlays();

    // Layout Mode
    bool layoutEnabled();
    bool hideDecoration();
    bool showAnimatedDecoration();
    bool recolorObjects();
    bool forceOpacity();
    bool normalBlending();
    bool hideGlow();
    bool hideParticles();
    bool hideEffects();
    bool hideMiddleground();
    bool disableShaders();
    bool alwaysShowPlayer();
    cocos2d::ccColor3B objectColor();
    cocos2d::ccColor3B detailColor();
    cocos2d::ccColor3B backgroundColor();
    cocos2d::ccColor3B groundColor();
    cocos2d::ccColor3B lineColor();

    // Screen-only overlays (hitboxes, trajectory)
    bool showHitboxes();
    bool showTrajectory();
    bool highPerformanceTrajectory();
    int trajectoryTps();
    float trajectoryLength();
    float trajectoryWidth();
    float hitboxWidth();
    float hitboxFillOpacity();
    cocos2d::ccColor4F color(Color key);

    // Debug
    bool debugLogging();
    int statsInterval();
    bool gpuTimers();

    // Incremented whenever any setting changes. Caches derived from settings
    // compare against it instead of re-reading Geode setting objects per frame.
    std::uint32_t generation();

    void logSummary();
}

#pragma once

#include <Geode/Geode.hpp>

namespace cleanfeed::settings {
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

    bool enabled();
    bool captureCursor();
    bool captureSystemOverlays();
    bool hideXdbotUI();
    std::string const& senderName();
    bool showHitboxes();
    bool showTrajectory();
    bool highPerformanceTrajectory();
    int trajectoryTps();
    float trajectoryLength();
    float trajectoryWidth();
    float hitboxWidth();
    float hitboxFillOpacity();

    cocos2d::ccColor4F color(Color key);
}

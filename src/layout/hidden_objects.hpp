#pragma once

#include <Geode/Geode.hpp>

namespace layoutfeed::hidden {
    // Objects made invisible by alpha triggers, fades or the editor "Hide"
    // option have no geometry in the game's batches (cocos2d zeroes the quads
    // of invisible sprites), so the layout shader cannot reveal them. A node
    // at the top of the object layer rebuilds their quads from the object
    // transforms during the layout pass only.
    void attach(GJBaseGameLayer* layer);
    void detach();

    // Debug: logs how every kind of object on screen is classified and drawn
    // in the next layout frame (bound to a keybind).
    void requestDump();
    void dumpIfRequested(GJBaseGameLayer* layer);
}

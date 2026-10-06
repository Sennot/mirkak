#pragma once

#include <Geode/Geode.hpp>

namespace cleanfeed {
    class OverlayDrawNode final : public cocos2d::CCDrawNode {
    public:
        static OverlayDrawNode* create(bool (*enabled)()) {
            auto* result = new OverlayDrawNode(enabled);
            if (result->init()) {
                result->autorelease();
                result->m_bUseArea = false;
                return result;
            }
            delete result;
            return nullptr;
        }

        void visit() override {
            // Rendering continues during the death delay and pause, even when
            // updateVisibility no longer runs. Never draw stale enabled geometry
            // after a keybind/settings change in those frames.
            if (m_enabled()) cocos2d::CCDrawNode::visit();
        }

    private:
        explicit OverlayDrawNode(bool (*enabled)()) : m_enabled(enabled) {}
        bool (*m_enabled)();
    };
}

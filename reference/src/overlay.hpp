#pragma once

#include <Geode/Geode.hpp>

namespace cleanfeed::overlay {
    void attach(GJBaseGameLayer* layer);
    void detach(GJBaseGameLayer* layer);
    void drawForPlayer();
    cocos2d::CCNode* root();
    GJBaseGameLayer* layer();
}

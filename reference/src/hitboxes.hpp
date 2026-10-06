#pragma once

#include <Geode/Geode.hpp>

namespace cleanfeed::hitboxes {
    void attach(GJBaseGameLayer* layer);
    void detach(GJBaseGameLayer* layer);
    void update(GJBaseGameLayer* layer);
}

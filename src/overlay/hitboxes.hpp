#pragma once

#include <Geode/Geode.hpp>

namespace layoutfeed::hitboxes {
    void attach(GJBaseGameLayer* layer);
    void detach(GJBaseGameLayer* layer);
    void update(GJBaseGameLayer* layer);
}

#include "render_pass.hpp"

namespace cleanfeed::render {
    namespace {
        thread_local unsigned int s_playerOverlayDepth = 0;
    }

    bool isPlayerOverlayPass() {
        return s_playerOverlayDepth != 0;
    }

    PlayerOverlayPass::PlayerOverlayPass() {
        ++s_playerOverlayDepth;
    }

    PlayerOverlayPass::~PlayerOverlayPass() {
        --s_playerOverlayDepth;
    }
}

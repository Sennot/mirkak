#pragma once

namespace cleanfeed::render {
    bool isPlayerOverlayPass();

    class PlayerOverlayPass final {
    public:
        PlayerOverlayPass();
        ~PlayerOverlayPass();

        PlayerOverlayPass(PlayerOverlayPass const&) = delete;
        PlayerOverlayPass& operator=(PlayerOverlayPass const&) = delete;
    };
}

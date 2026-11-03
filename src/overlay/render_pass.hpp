#pragma once

namespace layoutfeed::render {
    bool isPlayerOverlayPass();

    class PlayerOverlayPass final {
    public:
        PlayerOverlayPass();
        ~PlayerOverlayPass();

        PlayerOverlayPass(PlayerOverlayPass const&) = delete;
        PlayerOverlayPass& operator=(PlayerOverlayPass const&) = delete;
    };
}

#pragma once

#include <Geode/Geode.hpp>

namespace cleanfeed {
    class SpoutSender final {
    public:
        static SpoutSender& get();

        void captureBackBuffer();
        void shutdown();

    private:
        SpoutSender() = default;

        bool ensureCompositionTarget(unsigned int width, unsigned int height);
        bool ensureCursorTexture(unsigned int width, unsigned int height);
        void releaseCompositionTarget();
        void releaseSender();

        void* m_spout = nullptr;
        std::string m_senderName;
        unsigned int m_compositionFbo = 0;
        unsigned int m_compositionTexture = 0;
        unsigned int m_compositionWidth = 0;
        unsigned int m_compositionHeight = 0;
        unsigned int m_cursorTexture = 0;
        unsigned int m_cursorWidth = 0;
        unsigned int m_cursorHeight = 0;
        bool m_cursorTextureNeedsUpload = true;
        bool m_warnedCursorFailure = false;
        bool m_warnedSendFailure = false;
        bool m_loggedPublishing = false;
    };
}

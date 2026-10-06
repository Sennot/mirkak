#pragma once

#include "settings.hpp"

#include <Geode/Geode.hpp>

#include <cstdint>

// Debug messages go through log::info so they appear without changing the
// Geode console log level. Every call site uses a string literal format.
#define LF_DEBUG(...)                                                  \
    do {                                                               \
        if (::layoutfeed::settings::debugLogging()) {                  \
            ::geode::log::info("[debug] " __VA_ARGS__);                \
        }                                                              \
    } while (false)

namespace layoutfeed::debug {
    enum class Timer : std::uint8_t {
        Spout,
        Layout,
        Count,
    };

    // Per-frame counters, summed and reported every statistics interval.
    struct Counters {
        std::uint32_t layoutFrames = 0;
        std::uint32_t objectBatchDraws = 0;
        std::uint32_t glowBatchesSkipped = 0;
        std::uint32_t flagRebuilds = 0;
        std::uint32_t flagReuses = 0;
        std::uint64_t quadsDrawn = 0;
        std::uint64_t quadsHidden = 0;
        std::uint32_t spritesTinted = 0;
        std::uint32_t spritesHidden = 0;
        std::uint32_t particlesSkipped = 0;
        std::uint32_t fallbackDraws = 0;
    };

    Counters& counters();

    // CPU timing with QueryPerformanceCounter.
    class CpuScope final {
    public:
        explicit CpuScope(Timer timer);
        ~CpuScope();
        CpuScope(CpuScope const&) = delete;
        CpuScope& operator=(CpuScope const&) = delete;

    private:
        Timer m_timer;
        std::int64_t m_start = 0;
    };

    // GPU timing with GL_TIME_ELAPSED queries in a small ring, read back a few
    // frames later so the CPU never waits for the GPU. Scopes must not nest.
    class GpuScope final {
    public:
        explicit GpuScope(Timer timer);
        ~GpuScope();
        GpuScope(GpuScope const&) = delete;
        GpuScope& operator=(GpuScope const&) = delete;

    private:
        bool m_active = false;
    };

    // Logs every pending OpenGL error once per call site (rate limited).
    void checkGL(char const* where);

    // Called once per presented frame. Emits the statistics line when the
    // configured interval has elapsed.
    void endFrame();

    void logGLInfo();
}

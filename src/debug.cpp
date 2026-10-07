#include "debug.hpp"

#include <Geode/cocos/platform/win32/CCGL.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <unordered_map>

using namespace geode::prelude;

namespace layoutfeed::debug {
    namespace {
        constexpr std::size_t kTimerCount = static_cast<std::size_t>(Timer::Count);
        constexpr std::size_t kQueryRing = 4;

        char const* timerName(std::size_t index) {
            switch (static_cast<Timer>(index)) {
                case Timer::Spout: return "spout";
                case Timer::Layout: return "layout";
                case Timer::Overlay: return "menus";
                default: return "?";
            }
        }

        struct TimerStats {
            double cpuMs = 0.0;
            double cpuMaxMs = 0.0;
            std::uint32_t cpuSamples = 0;
            double gpuMs = 0.0;
            std::uint32_t gpuSamples = 0;
        };

        struct GpuRing {
            std::array<GLuint, kQueryRing> queries{};
            std::array<bool, kQueryRing> pending{};
            std::size_t next = 0;
            bool created = false;
        };

        Counters s_counters;
        std::array<TimerStats, kTimerCount> s_timers{};
        std::array<GpuRing, kTimerCount> s_gpu{};
        bool s_gpuUnavailable = false;
        bool s_gpuActive = false;
        std::size_t s_gpuActiveTimer = 0;

        std::uint32_t s_frames = 0;
        std::int64_t s_windowStart = 0;
        std::unordered_map<std::string, std::uint32_t> s_glErrorCounts;

        std::int64_t now() {
            LARGE_INTEGER value{};
            QueryPerformanceCounter(&value);
            return value.QuadPart;
        }

        double toMs(std::int64_t ticks) {
            static auto const frequency = [] {
                LARGE_INTEGER value{};
                QueryPerformanceFrequency(&value);
                return static_cast<double>(value.QuadPart);
            }();
            return static_cast<double>(ticks) * 1000.0 / frequency;
        }

        bool gpuTimersUsable() {
            if (s_gpuUnavailable) return false;
            if (!glGenQueries || !glBeginQuery || !glEndQuery || !glGetQueryObjectiv ||
                !glGetQueryObjectui64v) {
                s_gpuUnavailable = true;
                log::warn("GPU timer queries are unavailable on this driver; GPU timings disabled");
                return false;
            }
            return true;
        }

        // Collects finished queries without blocking.
        void harvest(std::size_t timer) {
            auto& ring = s_gpu[timer];
            if (!ring.created) return;
            for (std::size_t i = 0; i < kQueryRing; ++i) {
                if (!ring.pending[i]) continue;
                GLint available = 0;
                glGetQueryObjectiv(ring.queries[i], GL_QUERY_RESULT_AVAILABLE, &available);
                if (!available) continue;
                GLuint64 nanoseconds = 0;
                glGetQueryObjectui64v(ring.queries[i], GL_QUERY_RESULT, &nanoseconds);
                ring.pending[i] = false;
                s_timers[timer].gpuMs += static_cast<double>(nanoseconds) / 1'000'000.0;
                ++s_timers[timer].gpuSamples;
            }
        }
    }

    Counters& counters() {
        return s_counters;
    }

    CpuScope::CpuScope(Timer timer) : m_timer(timer) {
        if (settings::debugLogging()) m_start = now();
    }

    CpuScope::~CpuScope() {
        if (!m_start) return;
        auto const elapsed = toMs(now() - m_start);
        auto& stats = s_timers[static_cast<std::size_t>(m_timer)];
        stats.cpuMs += elapsed;
        stats.cpuMaxMs = std::max(stats.cpuMaxMs, elapsed);
        ++stats.cpuSamples;
    }

    GpuScope::GpuScope(Timer timer) {
        if (!settings::debugLogging() || !settings::gpuTimers() || s_gpuActive) return;
        if (!gpuTimersUsable()) return;

        auto const index = static_cast<std::size_t>(timer);
        auto& ring = s_gpu[index];
        if (!ring.created) {
            glGenQueries(static_cast<GLsizei>(kQueryRing), ring.queries.data());
            ring.created = true;
        }
        harvest(index);
        // Skip this sample when every query of the ring is still in flight.
        if (ring.pending[ring.next]) return;

        glBeginQuery(GL_TIME_ELAPSED, ring.queries[ring.next]);
        s_gpuActive = true;
        s_gpuActiveTimer = index;
        m_active = true;
    }

    GpuScope::~GpuScope() {
        if (!m_active) return;
        glEndQuery(GL_TIME_ELAPSED);
        auto& ring = s_gpu[s_gpuActiveTimer];
        ring.pending[ring.next] = true;
        ring.next = (ring.next + 1) % kQueryRing;
        s_gpuActive = false;
    }

    void drainGL(char const* where) {
        if (settings::debugLogging()) {
            checkGL(where);
            return;
        }
        for (int guard = 0; guard < 16 && glGetError() != GL_NO_ERROR; ++guard) {}
    }

    void checkGL(char const* where) {
        if (!settings::debugLogging()) return;
        for (int guard = 0; guard < 16; ++guard) {
            auto const error = glGetError();
            if (error == GL_NO_ERROR) return;
            auto key = std::string(where) + "#" + std::to_string(error);
            auto& count = s_glErrorCounts[key];
            ++count;
            // 1st, 10th, 100th, ... occurrence keeps the log readable.
            if (count == 1 || count == 10 || count == 100 || count % 1000 == 0) {
                log::warn("[debug] OpenGL error 0x{:04X} at {} (seen {} times)", error, where, count);
            }
        }
    }

    void endFrame() {
        if (!settings::debugLogging()) {
            s_windowStart = 0;
            return;
        }

        ++s_frames;
        auto const current = now();
        if (!s_windowStart) {
            s_windowStart = current;
            s_frames = 0;
            s_counters = {};
            s_timers = {};
            return;
        }

        for (std::size_t i = 0; i < kTimerCount; ++i) {
            if (!s_gpuActive) harvest(i);
        }

        auto const windowMs = toMs(current - s_windowStart);
        if (windowMs < settings::statsInterval() * 1000.0) return;

        auto const fps = s_frames * 1000.0 / windowMs;
        log::info("[stats] {:.1f} fps over {:.1f}s ({} frames)", fps, windowMs / 1000.0, s_frames);
        for (std::size_t i = 0; i < kTimerCount; ++i) {
            auto const& stats = s_timers[i];
            if (!stats.cpuSamples) continue;
            char gpu[32] = "n/a";
            if (stats.gpuSamples) {
                std::snprintf(gpu, sizeof(gpu), "%.3f ms", stats.gpuMs / stats.gpuSamples);
            }
            log::info(
                "[stats]   {:<6} cpu avg {:.3f} ms, max {:.3f} ms | gpu avg {}",
                timerName(i), stats.cpuMs / stats.cpuSamples, stats.cpuMaxMs, gpu
            );
        }

        auto const& c = s_counters;
        if (c.layoutFrames) {
            auto const frames = static_cast<double>(c.layoutFrames);
            log::info(
                "[stats]   layout per frame: {:.1f} object batches, {:.1f} glow batches, "
                "{:.0f} quads drawn, {:.0f} quads hidden, {:.1f} sprites tinted, "
                "{:.1f} sprites hidden, {:.1f} particle systems and {:.1f} effects skipped, "
                "{:.1f} hidden objects revealed of {:.1f} gameplay objects in view",
                c.objectBatchDraws / frames, c.glowBatches / frames,
                c.quadsDrawn / frames, c.quadsHidden / frames, c.spritesTinted / frames,
                c.spritesHidden / frames, c.particlesSkipped / frames, c.effectsSkipped / frames,
                c.objectsRevealed / frames, c.revealCandidates / frames
            );
            auto const flagTotal = c.flagRebuilds + c.flagReuses;
            log::info(
                "[stats]   role flags: {} rebuilds, {} reuses ({:.1f}% cached), {} fallback draws",
                c.flagRebuilds, c.flagReuses,
                flagTotal ? 100.0 * c.flagReuses / flagTotal : 0.0, c.fallbackDraws
            );
            log::info(
                "[stats]   special layers per frame: {:.1f} sprites in the normal frame, {:.1f} in layout, "
                "{:.1f} containers redrawn by the layout pass",
                static_cast<double>(c.containerSpritesNormal) / frames,
                static_cast<double>(c.containerSpritesLayout) / frames, c.containerFallbacks / frames
            );
            auto const spriteTotal = c.spritesResolved + c.spritesReused;
            log::info(
                "[stats]   rebuilt sprites: {} roles resolved, {} reused ({:.1f}% reused)",
                c.spritesResolved, c.spritesReused,
                spriteTotal ? 100.0 * static_cast<double>(c.spritesReused) / static_cast<double>(spriteTotal) : 0.0
            );
        }

        s_windowStart = current;
        s_frames = 0;
        s_counters = {};
        s_timers = {};
    }

    void logGLInfo() {
        auto const text = [](GLenum name) {
            auto const* value = reinterpret_cast<char const*>(glGetString(name));
            return value ? std::string(value) : std::string("<null>");
        };
        GLint major = 0;
        GLint minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        log::info(
            "OpenGL: vendor='{}' renderer='{}' version='{}' ({}.{}) glsl='{}'",
            text(GL_VENDOR), text(GL_RENDERER), text(GL_VERSION), major, minor,
            text(GL_SHADING_LANGUAGE_VERSION)
        );
    }
}

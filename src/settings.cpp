#include "settings.hpp"

#include <Geode/loader/SettingV3.hpp>

#include <algorithm>
#include <array>
#include <string_view>

using namespace geode::prelude;

namespace layoutfeed::settings {
    namespace {
        constexpr std::array<std::string_view, static_cast<std::size_t>(Color::Count)> kColorKeys{
            "solid-color", "hazard-color", "interactable-color",
            "player-color", "player-inner-color", "player-rotated-color",
            "trajectory-hold-color", "trajectory-release-color",
        };

        struct Cache {
            bool enabled = true;
            std::string senderName;
            bool captureCursor = true;
            bool captureMenus = true;
            bool captureSystemOverlays = false;

            bool layoutEnabled = true;
            bool hideDecoration = true;
            bool showAnimatedDecoration = true;
            bool recolorObjects = true;
            bool forceOpacity = true;
            bool normalBlending = true;
            bool hideGlow = true;
            bool hideParticles = true;
            bool hideEffects = true;
            bool hideMiddleground = true;
            bool disableShaders = true;
            bool alwaysShowPlayer = true;
            ccColor3B objectColor{255, 255, 255};
            ccColor3B detailColor{0, 0, 0};
            ccColor3B backgroundColor{40, 125, 255};
            ccColor3B groundColor{0, 102, 255};
            ccColor3B lineColor{255, 255, 255};

            bool showHitboxes = true;
            bool showTrajectory = true;
            bool highPerformanceTrajectory = true;
            int trajectoryTps = 240;
            float trajectoryLength = 2.f;
            float trajectoryWidth = 0.65f;
            float hitboxWidth = 0.3f;
            float hitboxFillOpacity = 0.08f;
            std::array<ccColor4F, kColorKeys.size()> colors{};

            bool debugLogging = false;
            int statsInterval = 5;
            bool gpuTimers = true;
        };

        std::uint32_t s_generation = 1;

        Cache readSettings() {
            auto* mod = Mod::get();
            Cache value;
            value.enabled = mod->getSettingValue<bool>("enabled");
            value.senderName = mod->getSettingValue<std::string>("sender-name");
            if (value.senderName.empty()) value.senderName = "Geometry Dash";
            value.captureCursor = mod->getSettingValue<bool>("capture-cursor");
            value.captureMenus = mod->getSettingValue<bool>("capture-menus");
            value.captureSystemOverlays = mod->getSettingValue<bool>("capture-system-overlays");

            value.layoutEnabled = mod->getSettingValue<bool>("layout-enabled");
            value.hideDecoration = mod->getSettingValue<bool>("layout-hide-decoration");
            value.showAnimatedDecoration = mod->getSettingValue<bool>("layout-show-animated-decoration");
            value.recolorObjects = mod->getSettingValue<bool>("layout-recolor-objects");
            value.forceOpacity = mod->getSettingValue<bool>("layout-force-opacity");
            value.normalBlending = mod->getSettingValue<bool>("layout-normal-blending");
            value.hideGlow = mod->getSettingValue<bool>("layout-hide-glow");
            value.hideParticles = mod->getSettingValue<bool>("layout-hide-particles");
            value.hideEffects = mod->getSettingValue<bool>("layout-hide-effects");
            value.hideMiddleground = mod->getSettingValue<bool>("layout-hide-middleground");
            value.disableShaders = mod->getSettingValue<bool>("layout-disable-shaders");
            value.alwaysShowPlayer = mod->getSettingValue<bool>("layout-always-show-player");
            value.objectColor = mod->getSettingValue<ccColor3B>("layout-object-color");
            value.detailColor = mod->getSettingValue<ccColor3B>("layout-detail-color");
            value.backgroundColor = mod->getSettingValue<ccColor3B>("layout-background-color");
            value.groundColor = mod->getSettingValue<ccColor3B>("layout-ground-color");
            value.lineColor = mod->getSettingValue<ccColor3B>("layout-line-color");

            value.showHitboxes = mod->getSettingValue<bool>("show-hitboxes");
            value.showTrajectory = mod->getSettingValue<bool>("show-trajectory");
            value.highPerformanceTrajectory = mod->getSettingValue<bool>("high-performance-trajectory");
            value.trajectoryTps = static_cast<int>(std::clamp(
                mod->getSettingValue<int64_t>("trajectory-tps"), int64_t{60}, int64_t{1000}
            ));
            value.trajectoryLength = std::clamp(static_cast<float>(mod->getSettingValue<double>("trajectory-length")), 0.1f, 5.f);
            value.trajectoryWidth = std::clamp(static_cast<float>(mod->getSettingValue<double>("trajectory-width")), 0.1f, 5.f);
            value.hitboxWidth = std::clamp(static_cast<float>(mod->getSettingValue<double>("hitbox-width")), 0.05f, 5.f);
            value.hitboxFillOpacity = std::clamp(static_cast<float>(mod->getSettingValue<double>("hitbox-fill-opacity")), 0.f, 1.f);
            constexpr float scale = 1.f / 255.f;
            for (std::size_t index = 0; index < kColorKeys.size(); ++index) {
                auto const color = mod->getSettingValue<ccColor4B>(std::string(kColorKeys[index]));
                value.colors[index] = {color.r * scale, color.g * scale, color.b * scale, color.a * scale};
            }

            value.debugLogging = mod->getSettingValue<bool>("debug-logging");
            value.statsInterval = static_cast<int>(std::clamp(
                mod->getSettingValue<int64_t>("debug-stats-interval"), int64_t{1}, int64_t{60}
            ));
            value.gpuTimers = mod->getSettingValue<bool>("debug-gpu-timers");
            return value;
        }

        Cache const& cache() {
            // Settings change on the main thread only. The per-frame render
            // path reads this cache and never resolves Geode setting objects.
            static Cache value = readSettings();
            static auto* listener = listenForAllSettingChanges(
                [](std::string_view key, std::shared_ptr<SettingV3>) {
                    value = readSettings();
                    ++s_generation;
                    if (value.debugLogging) {
                        log::info("[debug] setting '{}' changed (generation {})", key, s_generation);
                    }
                }
            );
            (void)listener;
            return value;
        }
    }

    bool enabled() { return cache().enabled; }
    std::string const& senderName() { return cache().senderName; }
    bool captureCursor() { return cache().captureCursor; }
    bool captureMenus() { return cache().captureMenus; }
    bool captureSystemOverlays() { return cache().captureSystemOverlays; }

    bool layoutEnabled() { return cache().layoutEnabled; }
    bool hideDecoration() { return cache().hideDecoration; }
    bool showAnimatedDecoration() { return cache().showAnimatedDecoration; }
    bool recolorObjects() { return cache().recolorObjects; }
    bool forceOpacity() { return cache().forceOpacity; }
    bool normalBlending() { return cache().normalBlending; }
    bool hideGlow() { return cache().hideGlow; }
    bool hideParticles() { return cache().hideParticles; }
    bool hideEffects() { return cache().hideEffects; }
    bool hideMiddleground() { return cache().hideMiddleground; }
    bool disableShaders() { return cache().disableShaders; }
    bool alwaysShowPlayer() { return cache().alwaysShowPlayer; }
    ccColor3B objectColor() { return cache().objectColor; }
    ccColor3B detailColor() { return cache().detailColor; }
    ccColor3B backgroundColor() { return cache().backgroundColor; }
    ccColor3B groundColor() { return cache().groundColor; }
    ccColor3B lineColor() { return cache().lineColor; }

    bool showHitboxes() { return cache().showHitboxes; }
    bool showTrajectory() { return cache().showTrajectory; }
    bool highPerformanceTrajectory() { return cache().highPerformanceTrajectory; }
    int trajectoryTps() { return cache().trajectoryTps; }
    float trajectoryLength() { return cache().trajectoryLength; }
    float trajectoryWidth() { return cache().trajectoryWidth; }
    float hitboxWidth() { return cache().hitboxWidth; }
    float hitboxFillOpacity() { return cache().hitboxFillOpacity; }
    ccColor4F color(Color key) { return cache().colors[static_cast<std::size_t>(key)]; }

    bool debugLogging() { return cache().debugLogging; }
    int statsInterval() { return cache().statsInterval; }
    bool gpuTimers() { return cache().gpuTimers; }

    std::uint32_t generation() {
        (void)cache();
        return s_generation;
    }

    void logSummary() {
        auto const& value = cache();
        log::info(
            "Settings: spout={} sender='{}' cursor={} menus={} steam={} | layout={} deco={} animDeco={} recolor={} opacity={} "
            "blending={} glow={} particles={} effects={} mg={} shaders={} player={} | hitboxes={} trajectory={} tps={} length={} | debug={} interval={}s gpuTimers={}",
            value.enabled, value.senderName, value.captureCursor, value.captureMenus,
            value.captureSystemOverlays,
            value.layoutEnabled, value.hideDecoration, value.showAnimatedDecoration, value.recolorObjects, value.forceOpacity,
            value.normalBlending, value.hideGlow, value.hideParticles, value.hideEffects, value.hideMiddleground,
            value.disableShaders, value.alwaysShowPlayer, value.showHitboxes, value.showTrajectory,
            value.trajectoryTps, value.trajectoryLength, value.debugLogging, value.statsInterval, value.gpuTimers
        );
    }
}

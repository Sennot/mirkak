#include "settings.hpp"

#include <Geode/loader/SettingV3.hpp>

#include <algorithm>

using namespace geode::prelude;

namespace layoutfeed::settings {
    namespace {
        struct Cache {
            bool enabled = true;
            std::string senderName;
            bool captureCursor = true;
            bool captureMenus = true;

            bool layoutEnabled = true;
            bool hideDecoration = true;
            bool recolorObjects = true;
            bool forceOpacity = true;
            bool normalBlending = true;
            bool hideGlow = true;
            bool hideParticles = true;
            bool hideMiddleground = true;
            bool disableShaders = true;
            ccColor3B objectColor{255, 255, 255};
            ccColor3B detailColor{0, 0, 0};
            ccColor3B backgroundColor{40, 125, 255};
            ccColor3B groundColor{0, 102, 255};
            ccColor3B lineColor{255, 255, 255};

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

            value.layoutEnabled = mod->getSettingValue<bool>("layout-enabled");
            value.hideDecoration = mod->getSettingValue<bool>("layout-hide-decoration");
            value.recolorObjects = mod->getSettingValue<bool>("layout-recolor-objects");
            value.forceOpacity = mod->getSettingValue<bool>("layout-force-opacity");
            value.normalBlending = mod->getSettingValue<bool>("layout-normal-blending");
            value.hideGlow = mod->getSettingValue<bool>("layout-hide-glow");
            value.hideParticles = mod->getSettingValue<bool>("layout-hide-particles");
            value.hideMiddleground = mod->getSettingValue<bool>("layout-hide-middleground");
            value.disableShaders = mod->getSettingValue<bool>("layout-disable-shaders");
            value.objectColor = mod->getSettingValue<ccColor3B>("layout-object-color");
            value.detailColor = mod->getSettingValue<ccColor3B>("layout-detail-color");
            value.backgroundColor = mod->getSettingValue<ccColor3B>("layout-background-color");
            value.groundColor = mod->getSettingValue<ccColor3B>("layout-ground-color");
            value.lineColor = mod->getSettingValue<ccColor3B>("layout-line-color");

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

    bool layoutEnabled() { return cache().layoutEnabled; }
    bool hideDecoration() { return cache().hideDecoration; }
    bool recolorObjects() { return cache().recolorObjects; }
    bool forceOpacity() { return cache().forceOpacity; }
    bool normalBlending() { return cache().normalBlending; }
    bool hideGlow() { return cache().hideGlow; }
    bool hideParticles() { return cache().hideParticles; }
    bool hideMiddleground() { return cache().hideMiddleground; }
    bool disableShaders() { return cache().disableShaders; }
    ccColor3B objectColor() { return cache().objectColor; }
    ccColor3B detailColor() { return cache().detailColor; }
    ccColor3B backgroundColor() { return cache().backgroundColor; }
    ccColor3B groundColor() { return cache().groundColor; }
    ccColor3B lineColor() { return cache().lineColor; }

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
            "Settings: spout={} sender='{}' cursor={} menus={} | layout={} deco={} recolor={} opacity={} "
            "blending={} glow={} particles={} mg={} shaders={} | debug={} interval={}s gpuTimers={}",
            value.enabled, value.senderName, value.captureCursor, value.captureMenus,
            value.layoutEnabled, value.hideDecoration, value.recolorObjects, value.forceOpacity,
            value.normalBlending, value.hideGlow, value.hideParticles, value.hideMiddleground,
            value.disableShaders, value.debugLogging, value.statsInterval, value.gpuTimers
        );
    }
}

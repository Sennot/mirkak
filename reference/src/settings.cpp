#include "settings.hpp"

#include <Geode/loader/SettingV3.hpp>

#include <algorithm>
#include <array>

using namespace geode::prelude;

namespace cleanfeed::settings {
    namespace {
        constexpr std::array<std::string_view, static_cast<size_t>(Color::Count)> kColorKeys{
            "solid-color", "hazard-color", "interactable-color",
            "player-color", "player-inner-color", "player-rotated-color",
            "trajectory-hold-color", "trajectory-release-color",
        };

        struct Cache {
            bool enabled;
            bool captureCursor;
            bool captureSystemOverlays;
            bool hideXdbotUI;
            bool showHitboxes;
            bool showTrajectory;
            bool highPerformanceTrajectory;
            int trajectoryTps;
            float trajectoryLength;
            float trajectoryWidth;
            float hitboxWidth;
            float hitboxFillOpacity;
            std::string senderName;
            std::array<cocos2d::ccColor4F, kColorKeys.size()> colors;
        };

        Cache readSettings() {
            auto* mod = Mod::get();
            Cache value{};
            value.enabled = mod->getSettingValue<bool>("enabled");
            value.captureCursor = mod->getSettingValue<bool>("capture-cursor");
            value.captureSystemOverlays = mod->getSettingValue<bool>("capture-system-overlays");
            value.hideXdbotUI = mod->getSettingValue<bool>("hide-xdbot-ui");
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
            value.senderName = mod->getSettingValue<std::string>("sender-name");
            if (value.senderName.empty()) value.senderName = "Geometry Dash Clean Feed";
            constexpr float scale = 1.f / 255.f;
            for (size_t index = 0; index < kColorKeys.size(); ++index) {
                auto const color = mod->getSettingValue<cocos2d::ccColor4B>(kColorKeys[index]);
                value.colors[index] = {color.r * scale, color.g * scale, color.b * scale, color.a * scale};
            }
            return value;
        }

        Cache const& cache() {
            // Settings, keybinds and palette application run on the main thread.
            // Refresh on their events, never by repeatedly resolving Geode's
            // setting objects in the per-frame drawing/prediction path.
            static Cache value = readSettings();
            static auto* listener = listenForAllSettingChanges(
                [](std::string_view, std::shared_ptr<SettingV3>) {
                    value = readSettings();
                }
            );
            (void)listener;
            return value;
        }
    }

    bool enabled() {
        return cache().enabled;
    }

    bool captureCursor() {
        return cache().captureCursor;
    }

    bool captureSystemOverlays() {
        return cache().captureSystemOverlays;
    }

    bool hideXdbotUI() {
        return cache().hideXdbotUI;
    }

    std::string const& senderName() {
        return cache().senderName;
    }

    bool showHitboxes() {
        return cache().showHitboxes;
    }

    bool showTrajectory() {
        return cache().showTrajectory;
    }

    bool highPerformanceTrajectory() {
        return cache().highPerformanceTrajectory;
    }

    int trajectoryTps() {
        return cache().trajectoryTps;
    }

    float trajectoryLength() {
        return cache().trajectoryLength;
    }

    float trajectoryWidth() {
        return cache().trajectoryWidth;
    }

    float hitboxWidth() {
        return cache().hitboxWidth;
    }

    float hitboxFillOpacity() {
        return cache().hitboxFillOpacity;
    }

    cocos2d::ccColor4F color(Color key) {
        return cache().colors[static_cast<size_t>(key)];
    }
}

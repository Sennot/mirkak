#include "overlay.hpp"
#include "overlay_draw_node.hpp"
#include "settings.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

void registerTestKeybinds();
GJBaseGameLayer* activeLayer = nullptr;
GJBaseGameLayer* cleanfeed::overlay::layer() { return activeLayer; }

void require(bool condition, char const* message) {
    if (!condition) throw std::runtime_error(message);
}

void press(char const* key, bool down = true, bool repeat = false) {
    geode::keybindListeners.at(key)(geode::Keybind{}, down, repeat, 0.0);
}

int main() {
    try {
        auto* mod = geode::Mod::get();
        mod->values = {
            {"enabled", true}, {"capture-cursor", true},
            {"hide-xdbot-ui", true}, {"capture-system-overlays", false},
            {"show-hitboxes", true}, {"show-trajectory", true},
            {"high-performance-trajectory", true}, {"trajectory-tps", int64_t{240}},
            {"trajectory-length", 2.0}, {"trajectory-width", 0.65},
            {"hitbox-width", 0.5}, {"hitbox-fill-opacity", 0.08},
            {"sender-name", std::string("Saved custom sender")},
        };
        char const* colorKeys[] = {
            "solid-color", "hazard-color", "interactable-color", "player-color",
            "player-inner-color", "player-rotated-color",
            "trajectory-hold-color", "trajectory-release-color",
        };
        for (auto key : colorKeys) mod->values[key] = cocos2d::ccColor4B{11, 22, 33, 44};

        using namespace cleanfeed;
        require(settings::senderName() == "Saved custom sender", "Saved settings were not loaded");
        require(settings::hideXdbotUI(), "xDBot filter default was not loaded");
        require(!settings::captureSystemOverlays(), "System overlay capture must default to off");
        mod->setSettingValue("capture-system-overlays", true);
        require(settings::captureSystemOverlays(), "System overlay capture changes were not applied");
        mod->setSettingValue("hide-xdbot-ui", false);
        require(!settings::hideXdbotUI(), "xDBot filter changes were not applied");
        require(settings::trajectoryTps() == 240 && settings::trajectoryLength() == 2.f,
            "Prediction defaults changed");
        registerTestKeybinds();
        GJBaseGameLayer level;
        activeLayer = &level;
        auto hitboxes = std::unique_ptr<OverlayDrawNode>(OverlayDrawNode::create(settings::showHitboxes));
        auto trajectory = std::unique_ptr<OverlayDrawNode>(OverlayDrawNode::create(settings::showTrajectory));

        // Only render visits happen below: the level is frozen after death or
        // paused. The same cached geometry must stop drawing on the next visit.
        hitboxes->visit();
        trajectory->visit();
        require(hitboxes->drawCalls == 1 && trajectory->drawCalls == 1, "Initial overlays missing");
        press("toggle-hitboxes-keybind");
        hitboxes->visit();
        trajectory->visit();
        require(hitboxes->drawCalls == 1 && trajectory->drawCalls == 2,
            "Hitbox toggle depends on gameplay updates or hides trajectory too");
        press("toggle-trajectory-keybind");
        hitboxes->visit();
        trajectory->visit();
        require(hitboxes->drawCalls == 1 && trajectory->drawCalls == 2,
            "Trajectory toggle depends on gameplay updates");

        press("toggle-trajectory-keybind", true, true);
        press("toggle-hitboxes-keybind", false);
        require(!settings::showHitboxes() && !settings::showTrajectory(), "Repeat/release toggled overlays");
        press("toggle-hitboxes-keybind");
        press("toggle-trajectory-keybind");
        hitboxes->visit();
        trajectory->visit();
        require(hitboxes->drawCalls == 2 && trajectory->drawCalls == 3, "Frozen geometry cannot be re-enabled");
        trajectory->setVisible(false);
        trajectory->visit();
        require(trajectory->drawCalls == 3, "Drawing gate overrides editor visibility");

        activeLayer = nullptr;
        press("toggle-hitboxes-keybind");
        require(settings::showHitboxes(), "Keybind changed settings outside a level");
        mod->setSettingValue("show-hitboxes", false);
        hitboxes->visit();
        require(hitboxes->drawCalls == 2, "Settings UI changes do not gate drawing");

        // Smart Contrast changes these existing settings one by one.
        for (int i = 0; i < 8; ++i) {
            mod->setSettingValue(colorKeys[i], cocos2d::ccColor4B{uint8_t(40 + i), 128, 255, 64});
            auto color = settings::color(static_cast<settings::Color>(i));
            require(std::abs(color.r - float(40 + i) / 255.f) < 0.00001f &&
                std::abs(color.a - 64.f / 255.f) < 0.00001f, "Palette change was cached incorrectly");
        }
        mod->setSettingValue("sender-name", std::string());
        require(settings::senderName() == "Geometry Dash Clean Feed", "Empty sender fallback lost");
        mod->setSettingValue("trajectory-tps", int64_t{10000000000LL});
        mod->setSettingValue("trajectory-length", 7.0);
        mod->setSettingValue("hitbox-fill-opacity", -1.0);
        require(settings::trajectoryTps() == 1000 && settings::trajectoryLength() == 5.f &&
            settings::hitboxFillOpacity() == 0.f, "Setting limits not preserved");

        auto const reads = mod->settingReads;
        for (int frame = 0; frame < 10000; ++frame) {
            (void)settings::enabled();
            (void)settings::captureCursor();
            (void)settings::hideXdbotUI();
            (void)settings::senderName();
            (void)settings::showHitboxes();
            (void)settings::showTrajectory();
            (void)settings::highPerformanceTrajectory();
            (void)settings::trajectoryTps();
            (void)settings::trajectoryLength();
            (void)settings::trajectoryWidth();
            (void)settings::hitboxWidth();
            (void)settings::hitboxFillOpacity();
            (void)settings::color(settings::Color::Solid);
            hitboxes->visit();
            trajectory->visit();
        }
        require(mod->settingReads == reads, "Per-frame settings lookups remain");
        std::cout << "PASS: death/pause toggles, independent overlays, key repeat, saved settings, "
            "palette updates, limits, 10000 frames without settings lookups\n";
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

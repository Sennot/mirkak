#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCEGLView.hpp>

#include "debug.hpp"
#include "layout/layout_pass.hpp"
#include "overlay_capture.hpp"
#include "settings.hpp"
#include "spout/spout_sender.hpp"

using namespace geode::prelude;

namespace layoutfeed {
    namespace {
        // Mod menus that draw inside their own swapBuffers hook. The two hooks
        // below are ordered around them, as in the reference clean-feed mod.
        constexpr char const* kMenuMods[] = {"absolllute.hackmega", "absolllute.megahack"};
        constexpr char const* kSwapHook = "cocos2d::CCEGLView::swapBuffers";

        enum class FrameMode {
            Plain,      // no layout: Spout2 captures after the menus
            Redirected, // layout + menus composited into both outputs
            Early,      // layout, menus kept out of Spout2 (or redirect unavailable)
        };

        FrameMode s_mode = FrameMode::Plain;

        void captureSpout() {
            debug::CpuScope cpu(debug::Timer::Spout);
            debug::GpuScope gpu(debug::Timer::Spout);
            spout::SpoutSender::get().captureBackBuffer();
        }

        // Runs before the menus draw: the back buffer holds the normal frame.
        void beforeMenus() {
            debug::checkGL("frame start (errors from the game or other mods)");
            if (overlay::redirecting()) overlay::abandon("swapBuffers");

            s_mode = FrameMode::Plain;
            if (!pass::shouldRender()) return;

            if (settings::captureMenus() && overlay::begin(&pass::render)) {
                s_mode = FrameMode::Redirected;
                return;
            }

            // Simple path: send the normal frame now, then draw the layout.
            s_mode = FrameMode::Early;
            captureSpout();
            debug::checkGL("spout::capture");
            pass::render();
        }

        // Runs after the menus have drawn, right before the real swap.
        void afterMenus() {
            switch (s_mode) {
                case FrameMode::Redirected:
                    overlay::finish();
                    break;
                case FrameMode::Plain:
                    captureSpout();
                    debug::checkGL("spout::capture");
                    break;
                case FrameMode::Early:
                    break;
            }
            s_mode = FrameMode::Plain;
            debug::endFrame();
        }
    }

    class $modify(LayoutFeedBeforeMenus, cocos2d::CCEGLView) {
        static void onModify(auto& self) {
            if (auto result = self.setHookPriority(kSwapHook, Priority::FirstPre); result.isErr()) {
                log::warn("Unable to set swapBuffers hook priority: {}", result.unwrapErr());
            }
            for (auto const id : kMenuMods) {
                if (auto* mod = Loader::get()->getInstalledMod(id)) {
                    (void)self.setHookPriorityBeforePre(kSwapHook, mod);
                }
            }
        }

        void swapBuffers() override {
            beforeMenus();
            cocos2d::CCEGLView::swapBuffers();
        }
    };

    class $modify(LayoutFeedAfterMenus, cocos2d::CCEGLView) {
        static void onModify(auto& self) {
            if (auto result = self.setHookPriority(kSwapHook, Priority::LastPre); result.isErr()) {
                log::warn("Unable to set swapBuffers hook priority: {}", result.unwrapErr());
            }
            for (auto const id : kMenuMods) {
                if (auto* mod = Loader::get()->getInstalledMod(id)) {
                    (void)self.setHookPriorityAfterPre(kSwapHook, mod);
                }
            }
        }

        void swapBuffers() override {
            afterMenus();
            cocos2d::CCEGLView::swapBuffers();
        }
    };

    class $modify(LayoutFeedDirector, cocos2d::CCDirector) {
        void drawScene() {
            // Never let a frame render into the overlay target if a previous
            // swap was skipped by another hook.
            if (overlay::redirecting()) overlay::abandon("drawScene");
            cocos2d::CCDirector::drawScene();
        }
    };
}

$on_mod(Loaded) {
    log::info("Spout2 Layout Feed {} loaded", Mod::get()->getVersion().toVString());
    layoutfeed::settings::logSummary();
    for (auto const id : layoutfeed::kMenuMods) {
        if (auto* mod = Loader::get()->getInstalledMod(id)) {
            log::info("Menu mod {} detected; swapBuffers hooks are ordered around it", mod->getID());
        }
    }
}

$on_game(Loaded) {
    listenForKeybindSettingPresses(
        "layout-keybind",
        [](Keybind const&, bool down, bool repeat, double) {
            if (!down || repeat) return;
            auto* mod = Mod::get();
            auto const enabled = !mod->getSettingValue<bool>("layout-enabled");
            mod->setSettingValue<bool>("layout-enabled", enabled);
            log::info("Layout Mode {} by keybind", enabled ? "enabled" : "disabled");
        }
    );
}
